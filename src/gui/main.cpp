#include "gui/editor_model.hpp"
#include "gui/frame_controller.hpp"
#include "gui/inspect_server.hpp"
#include "gui/inspection.hpp"
#include "gui/skia_presenter.hpp"
#if defined(__APPLE__)
#include "gui/mac_scroll_bridge.hpp"
#endif

#include <SDL3/SDL.h>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cind::gui {

namespace {

double elapsed_microseconds(std::chrono::steady_clock::time_point started,
                            std::chrono::steady_clock::time_point finished) {
    return std::chrono::duration<double, std::micro>(finished - started).count();
}

class HeadlessWakeup {
public:
    void notify() {
        {
            std::scoped_lock lock(mutex_);
            notified_ = true;
        }
        changed_.notify_one();
    }

    void wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return notified_; });
        notified_ = false;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool notified_ = false;
};

std::string_view cursor_owner_name(SkiaCursorOwner owner) {
    switch (owner) {
    case SkiaCursorOwner::None:
        return "none";
    case SkiaCursorOwner::Document:
        return "document";
    case SkiaCursorOwner::Popup:
        return "popup";
    case SkiaCursorOwner::Echo:
        return "echo";
    case SkiaCursorOwner::Other:
        return "other";
    }
    return "none";
}

KeyModifiers key_modifiers(SDL_Keymod modifiers) {
    KeyModifiers result;
    if ((modifiers & SDL_KMOD_CTRL) != 0) {
        result |= KeyModifier::Control;
    }
    if ((modifiers & SDL_KMOD_ALT) != 0) {
        result |= KeyModifier::Alt;
    }
    if ((modifiers & SDL_KMOD_SHIFT) != 0) {
        result |= KeyModifier::Shift;
    }
    if ((modifiers & SDL_KMOD_GUI) != 0) {
        result |= KeyModifier::Super;
    }
    return result;
}

std::optional<KeyStroke> editor_key(SDL_Scancode scancode, SDL_Keymod modifiers) {
    KeyModifiers mods = key_modifiers(modifiers);
    const auto punctuation = [&](char32_t plain, char32_t shifted) {
        char32_t character = plain;
        if (has_modifier(mods, KeyModifier::Shift)) {
            character = shifted;
            mods.bits &= ~static_cast<std::uint8_t>(KeyModifier::Shift);
        }
        return KeyStroke::character_key(character, mods);
    };
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        const char32_t character = U'a' + static_cast<char32_t>(scancode - SDL_SCANCODE_A);
        return KeyStroke::character_key(character, mods);
    }
    switch (scancode) {
    case SDL_SCANCODE_LEFT:
        return KeyStroke::named(KeyCode::Left, mods);
    case SDL_SCANCODE_RIGHT:
        return KeyStroke::named(KeyCode::Right, mods);
    case SDL_SCANCODE_UP:
        return KeyStroke::named(KeyCode::Up, mods);
    case SDL_SCANCODE_DOWN:
        return KeyStroke::named(KeyCode::Down, mods);
    case SDL_SCANCODE_HOME:
        return KeyStroke::named(KeyCode::Home, mods);
    case SDL_SCANCODE_END:
        return KeyStroke::named(KeyCode::End, mods);
    case SDL_SCANCODE_PAGEUP:
        return KeyStroke::named(KeyCode::PageUp, mods);
    case SDL_SCANCODE_PAGEDOWN:
        return KeyStroke::named(KeyCode::PageDown, mods);
    case SDL_SCANCODE_BACKSPACE:
        return KeyStroke::named(KeyCode::Backspace, mods);
    case SDL_SCANCODE_DELETE:
        return KeyStroke::named(KeyCode::Delete, mods);
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        return KeyStroke::named(KeyCode::Enter, mods);
    case SDL_SCANCODE_TAB:
        return KeyStroke::named(KeyCode::Tab, mods);
    case SDL_SCANCODE_ESCAPE:
        return KeyStroke::named(KeyCode::Escape, mods);
    case SDL_SCANCODE_SPACE:
        return KeyStroke::character_key(U' ', mods);
    case SDL_SCANCODE_SLASH:
        return punctuation(U'/', U'?');
    case SDL_SCANCODE_MINUS:
        return punctuation(U'-', U'_');
    case SDL_SCANCODE_EQUALS:
        return punctuation(U'=', U'+');
    case SDL_SCANCODE_LEFTBRACKET:
        return punctuation(U'[', U'{');
    case SDL_SCANCODE_RIGHTBRACKET:
        return punctuation(U']', U'}');
    case SDL_SCANCODE_BACKSLASH:
        return punctuation(U'\\', U'|');
    case SDL_SCANCODE_SEMICOLON:
        return punctuation(U';', U':');
    case SDL_SCANCODE_APOSTROPHE:
        return punctuation(U'\'', U'"');
    case SDL_SCANCODE_GRAVE:
        return punctuation(U'`', U'~');
    case SDL_SCANCODE_COMMA:
        return punctuation(U',', U'<');
    case SDL_SCANCODE_PERIOD:
        return punctuation(U'.', U'>');
    case SDL_SCANCODE_0:
        return punctuation(U'0', U')');
    case SDL_SCANCODE_1:
        return punctuation(U'1', U'!');
    case SDL_SCANCODE_2:
        return punctuation(U'2', U'@');
    case SDL_SCANCODE_3:
        return punctuation(U'3', U'#');
    case SDL_SCANCODE_4:
        return punctuation(U'4', U'$');
    case SDL_SCANCODE_5:
        return punctuation(U'5', U'%');
    case SDL_SCANCODE_6:
        return punctuation(U'6', U'^');
    case SDL_SCANCODE_7:
        return punctuation(U'7', U'&');
    case SDL_SCANCODE_8:
        return punctuation(U'8', U'*');
    case SDL_SCANCODE_9:
        return punctuation(U'9', U'(');
    default:
        return std::nullopt;
    }
}

class SdlRuntime {
public:
    SdlRuntime() {
#if defined(__APPLE__)
        SDL_SetHint(SDL_HINT_MAC_SCROLL_MOMENTUM, "1");
#else
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
#endif
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(
                std::format("SDL video initialization failed: {}", SDL_GetError()));
        }
        initialized_ = true;
#if !defined(__APPLE__)
        const char* driver = SDL_GetCurrentVideoDriver();
        if (!driver || std::string_view(driver) != "wayland") {
            SDL_Quit();
            initialized_ = false;
            throw std::runtime_error(
                std::format("SDL selected '{}' instead of Wayland", driver ? driver : "none"));
        }
#endif
    }

    ~SdlRuntime() {
        if (initialized_) {
            SDL_Quit();
        }
    }

    SdlRuntime(const SdlRuntime&) = delete;
    SdlRuntime& operator=(const SdlRuntime&) = delete;

private:
    bool initialized_ = false;
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
};

struct SdlMemoryDeleter {
    void operator()(void* memory) const { SDL_free(memory); }
};

struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const { SDL_DestroyRenderer(renderer); }
};

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
};

class SdlWindow {
public:
    SdlWindow(EditorModel& editor, SkiaPresenter& presenter, InspectionHub* inspection,
              Uint32 background_event)
        : editor_(editor), presenter_(presenter),
          frame_controller_(presenter, editor.presentation_motion()), inspection_(inspection),
          background_event_(background_event) {
        constexpr SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        window_.reset(SDL_CreateWindow("cind · Skia", 1100, 760, flags));
        if (!window_) {
            throw std::runtime_error(std::format("SDL window creation failed: {}", SDL_GetError()));
        }
        const PresentationMetrics& metrics = presenter_.metrics();
        SDL_SetWindowMinimumSize(
            window_.get(), presenter_.cell_width() * static_cast<int>(metrics.minimum_columns),
            presenter_.cell_height() * static_cast<int>(metrics.minimum_rows));
        renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
        if (!renderer_) {
            throw std::runtime_error(
                std::format("SDL renderer creation failed: {}", SDL_GetError()));
        }
        vsync_enabled_ = SDL_SetRenderVSync(renderer_.get(), 1);
        if (!SDL_StartTextInput(window_.get())) {
            throw std::runtime_error(std::format("SDL text input failed: {}", SDL_GetError()));
        }
#if defined(__APPLE__)
        mac_scroll_event_ = SDL_RegisterEvents(1);
        if (mac_scroll_event_ == 0) {
            throw std::runtime_error(
                std::format("SDL scroll event allocation failed: {}", SDL_GetError()));
        }
        void* native_window = SDL_GetPointerProperty(SDL_GetWindowProperties(window_.get()),
                                                     SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        mac_scroll_monitor_ = install_mac_scroll_monitor(
            native_window,
            [](MacScrollDelta delta, void* context) {
                static_cast<SdlWindow*>(context)->queue_mac_scroll(delta);
            },
            this);
        if (mac_scroll_monitor_ == nullptr) {
            throw std::runtime_error("failed to install the macOS precision scroll monitor");
        }
#endif
    }

    ~SdlWindow() {
#if defined(__APPLE__)
        uninstall_mac_scroll_monitor(mac_scroll_monitor_);
#endif
    }

    void run() {
        paint();
        while (!editor_.should_quit()) {
            SDL_Event event{};
            const bool animating = frame_controller_.animations_active();
            bool received = false;
            if (animating && vsync_enabled_) {
                received = SDL_PollEvent(&event);
            } else if (animating) {
                received = SDL_WaitEventTimeout(&event, 1);
            } else {
                received = SDL_WaitEvent(&event);
            }
            if (!received && !animating) {
                throw std::runtime_error(std::format("SDL event wait failed: {}", SDL_GetError()));
            }
            bool repaint = false;
            if (received) {
                repaint = dispatch_event(event).repaint;
                while (SDL_PollEvent(&event)) {
                    repaint = dispatch_event(event).repaint || repaint;
                }
            }
            repaint = editor_.poll_background_work() || repaint;
            repaint = frame_controller_.animations_active() || repaint;
            if (repaint && !editor_.should_quit()) {
                paint();
            }
        }
    }

private:
    struct EventResult {
        bool handled = false;
        bool repaint = false;
    };

    static FrameIdentity frame_identity(const EditorRenderState& editor) {
        return {.window_slot = editor.window_slot,
                .window_generation = editor.window_generation,
                .view_slot = editor.view_slot,
                .view_generation = editor.view_generation,
                .buffer_slot = editor.buffer_slot,
                .buffer_generation = editor.buffer_generation,
                .revision = editor.revision};
    }

    EventResult dispatch_event(const SDL_Event& event) {
        const RevisionId revision_before = editor_.revision();
        const EventResult result = handle_event(event);
        if (inspection_) {
            const std::uint64_t sequence =
                inspection_->record_event({.type = event_type(event),
                                           .detail = event_detail(event),
                                           .handled = result.handled,
                                           .repaint = result.repaint,
                                           .revision_before = revision_before,
                                           .revision_after = editor_.revision()});
            if (result.repaint) {
                last_repaint_event_sequence_ = sequence;
            }
        }
        return result;
    }

    EventResult handle_event(const SDL_Event& event) {
        if (event.type == background_event_) {
            return {true, false};
        }
#if defined(__APPLE__)
        if (event.type == mac_scroll_event_) {
            const float lines = pending_mac_scroll_lines_;
            const float steps = pending_mac_scroll_steps_;
            pending_mac_scroll_lines_ = 0.0F;
            pending_mac_scroll_steps_ = 0.0F;
            mac_scroll_event_queued_ = false;
            last_mac_scroll_lines_ = lines;
            last_mac_scroll_steps_ = steps;
            if (lines != 0.0F) {
                editor_.scroll_lines(lines);
                direct_scroll_pending_ = true;
            }
            if (steps != 0.0F) {
                editor_.scroll_steps(steps);
                direct_scroll_pending_ = true;
            }
            return {true, lines != 0.0F || steps != 0.0F};
        }
#endif
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            editor_.request_close();
            return {true, !editor_.should_quit()};
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            return {true, true};
        case SDL_EVENT_KEY_DOWN: {
            suppress_text_input_ = false;
            const std::optional<KeyStroke> key = editor_key(event.key.scancode, event.key.mod);
            const bool handled = key && editor_.handle_key(*key, page_rows_);
            constexpr SDL_Keymod command_modifiers =
                static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI);
            suppress_text_input_ = handled && key && key->code == KeyCode::Character &&
                                   (event.key.mod & command_modifiers) == 0;
            return {handled, handled};
        }
        case SDL_EVENT_TEXT_INPUT:
            if (suppress_text_input_) {
                suppress_text_input_ = false;
                return {true, false};
            }
            editor_.insert_text(event.text.text ? event.text.text : "");
            return {true, true};
        case SDL_EVENT_TEXT_EDITING:
            editor_.set_preedit(event.edit.text ? event.edit.text : "");
            return {true, true};
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                if (const std::optional<ui::HitTarget> target =
                        mouse_hit_target(event.button.x, event.button.y)) {
                    editor_.click(*target);
                }
                return {true, true};
            }
            return {};
        case SDL_EVENT_MOUSE_WHEEL: {
            float amount = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                amount = -amount;
            }
#if !defined(__APPLE__)
            amount = -amount;
#endif
            if (amount != 0.0F) {
                editor_.scroll_steps(amount);
#if defined(__APPLE__)
                direct_scroll_pending_ = true;
#endif
            }
            const bool handled = amount != 0.0F;
            return {handled, handled};
        }
        default:
            return {};
        }
    }

    std::string event_type(const SDL_Event& event) const {
        if (event.type == background_event_) {
            return "background-ready";
        }
#if defined(__APPLE__)
        if (event.type == mac_scroll_event_) {
            return "mac-scroll";
        }
#endif
        switch (event.type) {
        case SDL_EVENT_QUIT:
            return "quit";
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return "window-close";
        case SDL_EVENT_WINDOW_EXPOSED:
            return "window-exposed";
        case SDL_EVENT_WINDOW_RESIZED:
            return "window-resized";
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            return "window-pixel-size-changed";
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            return "window-display-changed";
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            return "window-display-scale-changed";
        case SDL_EVENT_WINDOW_RESTORED:
            return "window-restored";
        case SDL_EVENT_KEY_DOWN:
            return "key-down";
        case SDL_EVENT_TEXT_INPUT:
            return "text-input";
        case SDL_EVENT_TEXT_EDITING:
            return "text-editing";
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            return "mouse-button-down";
        case SDL_EVENT_MOUSE_WHEEL:
            return "mouse-wheel";
        default:
            return std::format("sdl-event-{}", event.type);
        }
    }

    std::string event_detail(const SDL_Event& event) const {
#if defined(__APPLE__)
        if (event.type == mac_scroll_event_) {
            return std::format("lines={} steps={}", last_mac_scroll_lines_, last_mac_scroll_steps_);
        }
#endif
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            return std::format(
                "scancode={} name={} modifiers={}", static_cast<int>(event.key.scancode),
                SDL_GetScancodeName(event.key.scancode), static_cast<unsigned int>(event.key.mod));
        case SDL_EVENT_TEXT_INPUT:
            return std::format("text={}", event.text.text ? event.text.text : "");
        case SDL_EVENT_TEXT_EDITING:
            return std::format("preedit={}", event.edit.text ? event.edit.text : "");
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const std::optional<ui::HitTarget> target =
                mouse_hit_target(event.button.x, event.button.y);
            if (!target) {
                return std::format("button={} window=({}, {}) target=none",
                                   static_cast<int>(event.button.button), event.button.x,
                                   event.button.y);
            }
            return std::format(
                "button={} window=({}, {}) target={} view={} line={} column={} item={}",
                static_cast<int>(event.button.button), event.button.x, event.button.y,
                ui::hit_target_kind_name(target->kind), target->view_id,
                target->document_line ? std::to_string(*target->document_line) : "none",
                target->display_column ? std::to_string(*target->display_column) : "none",
                target->popup_item ? std::to_string(*target->popup_item) : "none");
        }
        case SDL_EVENT_MOUSE_WHEEL:
            return std::format("delta=({}, {}) ticks=({}, {}) direction={}", event.wheel.x,
                               event.wheel.y, event.wheel.integer_x, event.wheel.integer_y,
                               static_cast<int>(event.wheel.direction));
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            return std::format("size={}x{}", event.window.data1, event.window.data2);
        default:
            return {};
        }
    }

    void ensure_texture(int width, int height) {
        if (texture_ && texture_width_ == width && texture_height_ == height) {
            return;
        }
        texture_.reset();
        scroll_texture_.reset();
        std::unique_ptr<SDL_Texture, TextureDeleter> primary(SDL_CreateTexture(
            renderer_.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height));
        std::unique_ptr<SDL_Texture, TextureDeleter> secondary(SDL_CreateTexture(
            renderer_.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height));
        const auto configure = [](SDL_Texture* texture) {
            return texture != nullptr && SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) &&
                   SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        };
        if (configure(primary.get()) && configure(secondary.get())) {
            texture_ = std::move(primary);
            scroll_texture_ = std::move(secondary);
        } else {
            texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, width, height));
        }
        if (!texture_) {
            throw std::runtime_error(
                std::format("SDL texture creation failed: {}", SDL_GetError()));
        }
        texture_width_ = width;
        texture_height_ = height;
    }

    bool translate_texture(const SkiaGridTranslation& translation, int pixel_width,
                           int pixel_height, const std::byte* pixel_bytes, std::size_t row_bytes,
                           RenderTimingSnapshot& timings) {
        if (!scroll_texture_) {
            return false;
        }
        SDL_Texture* const target = scroll_texture_.get();
        if (!SDL_SetRenderTarget(renderer_.get(), target)) {
            scroll_texture_.reset();
            return false;
        }

        const int output_rows = translation.output_rows;
        const int grid_bottom = translation.grid_output_bottom;
        const int overlap_height = grid_bottom - std::abs(output_rows);
        const float source_y = static_cast<float>(output_rows < 0 ? -output_rows : 0);
        const float destination_y = static_cast<float>(output_rows > 0 ? output_rows : 0);
        const SDL_FRect grid_source{.x = 0.0F,
                                    .y = source_y,
                                    .w = static_cast<float>(pixel_width),
                                    .h = static_cast<float>(overlap_height)};
        const SDL_FRect grid_destination{.x = 0.0F,
                                         .y = destination_y,
                                         .w = static_cast<float>(pixel_width),
                                         .h = static_cast<float>(overlap_height)};
        bool copied =
            SDL_RenderTexture(renderer_.get(), texture_.get(), &grid_source, &grid_destination);
        if (copied && grid_bottom < pixel_height) {
            const SDL_FRect footer{.x = 0.0F,
                                   .y = static_cast<float>(grid_bottom),
                                   .w = static_cast<float>(pixel_width),
                                   .h = static_cast<float>(pixel_height - grid_bottom)};
            copied = SDL_RenderTexture(renderer_.get(), texture_.get(), &footer, &footer);
        }
        if (!SDL_SetRenderTarget(renderer_.get(), nullptr)) {
            throw std::runtime_error(
                std::format("SDL render target restore failed: {}", SDL_GetError()));
        }
        if (!copied || !SDL_FlushRenderer(renderer_.get())) {
            scroll_texture_.reset();
            return false;
        }

        const SDL_Rect exposed{
            .x = 0,
            .y = output_rows < 0 ? grid_bottom + output_rows : 0,
            .w = pixel_width,
            .h = std::abs(output_rows),
        };
        const std::size_t byte_offset = static_cast<std::size_t>(exposed.y) * row_bytes;
        if (!SDL_UpdateTexture(target, &exposed, pixel_bytes + byte_offset,
                               static_cast<int>(row_bytes))) {
            scroll_texture_.reset();
            return false;
        }
        std::swap(texture_, scroll_texture_);
        timings.texture_scroll_reused = true;
        timings.texture_copy_pixels = static_cast<std::uint64_t>(pixel_width) *
                                      static_cast<std::uint64_t>(pixel_height - exposed.h);
        timings.uploaded_bytes += static_cast<std::uint64_t>(exposed.w) *
                                  static_cast<std::uint64_t>(exposed.h) * sizeof(std::uint32_t);
        ++timings.upload_rects;
        return true;
    }

    bool texture_matches_pixels(const std::byte* expected, int pixel_width, int pixel_height,
                                std::size_t row_bytes) const {
        std::unique_ptr<SDL_Surface, SurfaceDeleter> captured(
            SDL_RenderReadPixels(renderer_.get(), nullptr));
        if (!captured) {
            return false;
        }
        std::unique_ptr<SDL_Surface, SurfaceDeleter> converted(
            SDL_ConvertSurface(captured.get(), SDL_PIXELFORMAT_ARGB8888));
        if (!converted || converted->w != pixel_width || converted->h != pixel_height ||
            converted->pitch < pixel_width * static_cast<int>(sizeof(std::uint32_t))) {
            return false;
        }
        const auto* actual = static_cast<const std::byte*>(converted->pixels);
        const std::size_t compared_row_bytes =
            static_cast<std::size_t>(pixel_width) * sizeof(std::uint32_t);
        for (int y = 0; y < pixel_height; ++y) {
            if (std::memcmp(actual + static_cast<std::size_t>(y) *
                                         static_cast<std::size_t>(converted->pitch),
                            expected + static_cast<std::size_t>(y) * row_bytes,
                            compared_row_bytes) != 0) {
                return false;
            }
        }
        return true;
    }

    float display_scale() const {
        const float scale = SDL_GetWindowDisplayScale(window_.get());
        return scale > 0.0F ? scale : 1.0F;
    }

    void paint() {
        const auto frame_started = std::chrono::steady_clock::now();
        RenderTimingSnapshot timings;
        int pixel_width = 0;
        int pixel_height = 0;
        if (!SDL_GetRenderOutputSize(renderer_.get(), &pixel_width, &pixel_height) ||
            pixel_width <= 0 || pixel_height <= 0) {
            return;
        }

        const float scale = display_scale();
        logical_output_width_ = static_cast<float>(pixel_width) / scale;
        logical_output_height_ = static_cast<float>(pixel_height) / scale;
        const float cell_width = static_cast<float>(presenter_.cell_width());
        const float cell_height = static_cast<float>(presenter_.cell_height());
        rows_ = std::max(3, static_cast<int>(std::ceil(logical_output_height_ / cell_height)));
        columns_ = std::max(20, static_cast<int>(std::ceil(logical_output_width_ / cell_width)));
        const float footer_height = presenter_.status_bar_height() + presenter_.echo_area_height();
        const float text_height = std::max(0.0F, logical_output_height_ - footer_height);
        const float visible_text_rows =
            std::clamp(text_height / cell_height, 1.0F, static_cast<float>(rows_ - 2));
        page_rows_ = std::clamp(static_cast<int>(std::floor(text_height / cell_height + 0.0001F)),
                                1, rows_ - 2);
        const auto layout_started = std::chrono::steady_clock::now();
        editor_.layout_view(rows_, columns_, visible_text_rows);
        const auto layout_finished = std::chrono::steady_clock::now();
        timings.layout_us = elapsed_microseconds(layout_started, layout_finished);

        const auto compose_started = layout_finished;
        ui::Scene scene = editor_.compose(rows_, columns_, visible_text_rows);
        const auto compose_finished = std::chrono::steady_clock::now();
        timings.compose_us = elapsed_microseconds(compose_started, compose_finished);

        const std::size_t pixel_count =
            static_cast<std::size_t>(pixel_width) * static_cast<std::size_t>(pixel_height);
        const bool geometry_changed =
            texture_width_ != pixel_width || texture_height_ != pixel_height ||
            std::abs(rendered_scale_ - scale) > 0.0001F || pixels_.size() != pixel_count;
        pixels_.resize(pixel_count);
        const std::size_t row_bytes = static_cast<std::size_t>(pixel_width) * sizeof(std::uint32_t);
        if (row_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("SDL texture pitch exceeds the supported range");
        }

        const auto state_started = std::chrono::steady_clock::now();
        const EditorRenderState editor_state = editor_.render_state();
        const auto state_finished = std::chrono::steady_clock::now();
        timings.render_state_us = elapsed_microseconds(state_started, state_finished);
        std::optional<EditorStateSnapshot> editor_snapshot;
        if (inspection_) {
            const auto inspect_started = std::chrono::steady_clock::now();
            editor_snapshot = editor_.inspect();
            timings.inspect_us =
                elapsed_microseconds(inspect_started, std::chrono::steady_clock::now());
        }
        const float scroll_top = static_cast<float>(editor_state.viewport.top_line) +
                                 editor_state.viewport.top_line_offset;
        const SkiaShapeCacheStats shape_cache_before = presenter_.shape_cache_stats();
        const auto build_started = std::chrono::steady_clock::now();
        PresentedFrame frame =
            frame_controller_.build({.scene = std::move(scene),
                                     .identity = frame_identity(editor_state),
                                     .scroll_top = scroll_top,
                                     .logical_width = logical_output_width_,
                                     .logical_height = logical_output_height_,
                                     .output_width = pixel_width,
                                     .output_height = pixel_height,
                                     .display_scale = scale,
                                     .animate_scroll = !direct_scroll_pending_,
                                     .constrain_scroll_to_cursor = editor_state.reveal_caret,
                                     .geometry_changed = geometry_changed,
                                     .now = FrameClock::now()});
        timings.frame_build_us =
            elapsed_microseconds(build_started, std::chrono::steady_clock::now());
        const SkiaShapeCacheStats shape_cache_after = presenter_.shape_cache_stats();
        timings.shape_cache_hits = shape_cache_after.hits - shape_cache_before.hits;
        timings.shape_cache_misses = shape_cache_after.misses - shape_cache_before.misses;
        timings.shape_cache_evictions = shape_cache_after.evictions - shape_cache_before.evictions;
        timings.shape_cache_entries = shape_cache_after.entries;
        direct_scroll_pending_ = false;

        SkiaRenderDiagnostics render_diagnostics;
        std::optional<SkiaGridTranslation> grid_translation;
        const auto raster_started = std::chrono::steady_clock::now();
        if (frame.animated()) {
            presenter_.render_animated(frame.layout(), frame.animation(), pixel_width, pixel_height,
                                       pixels_.data(), row_bytes, scale,
                                       inspection_ ? &render_diagnostics : nullptr);
        } else if (frame.scene_damage().full_repaint) {
            presenter_.render(frame.layout(), pixel_width, pixel_height, pixels_.data(), row_bytes,
                              scale, inspection_ ? &render_diagnostics : nullptr);
        } else if (!frame.logical_damage().empty()) {
            grid_translation = presenter_.render_grid_translation_damage(
                frame.layout(), frame.scene_damage(), pixel_width, pixel_height, pixels_.data(),
                row_bytes, scale);
            if (!grid_translation) {
                presenter_.render_damage(frame.layout(), pixel_width, pixel_height, pixels_.data(),
                                         row_bytes, frame.logical_damage(), scale);
            }
        }
        timings.raster_us = elapsed_microseconds(raster_started, std::chrono::steady_clock::now());

        bool full_reference_match = true;
        const auto reference_started = std::chrono::steady_clock::now();
        if (inspection_ && frame.animated()) {
            diagnostic_pixels_.resize(pixel_count);
            presenter_.render_animated(frame.layout(), frame.animation(), pixel_width, pixel_height,
                                       diagnostic_pixels_.data(), row_bytes, scale);
            full_reference_match = diagnostic_pixels_ == pixels_;
        } else if (inspection_ && !frame.scene_damage().full_repaint) {
            diagnostic_pixels_.resize(pixel_count);
            presenter_.render(frame.layout(), pixel_width, pixel_height, diagnostic_pixels_.data(),
                              row_bytes, scale, &render_diagnostics);
            full_reference_match = diagnostic_pixels_ == pixels_;
        }
        timings.reference_us =
            elapsed_microseconds(reference_started, std::chrono::steady_clock::now());

        ensure_texture(pixel_width, pixel_height);
        const auto* pixel_bytes = reinterpret_cast<const std::byte*>(pixels_.data());
        const auto upload_started = std::chrono::steady_clock::now();
        const bool texture_translated =
            grid_translation && translate_texture(*grid_translation, pixel_width, pixel_height,
                                                  pixel_bytes, row_bytes, timings);
        if (!texture_translated) {
            for (const FrameDamageRect& rect : frame.damage()) {
                const SDL_Rect output_rect{.x = rect.output.x,
                                           .y = rect.output.y,
                                           .w = rect.output.width,
                                           .h = rect.output.height};
                const std::size_t byte_offset =
                    static_cast<std::size_t>(rect.output.y) * row_bytes +
                    static_cast<std::size_t>(rect.output.x) * sizeof(std::uint32_t);
                if (!SDL_UpdateTexture(texture_.get(), &output_rect, pixel_bytes + byte_offset,
                                       static_cast<int>(row_bytes))) {
                    throw std::runtime_error(
                        std::format("SDL texture update failed: {}", SDL_GetError()));
                }
                timings.uploaded_bytes += static_cast<std::uint64_t>(rect.output.width) *
                                          static_cast<std::uint64_t>(rect.output.height) *
                                          sizeof(std::uint32_t);
                ++timings.upload_rects;
            }
        }
        timings.upload_us = elapsed_microseconds(upload_started, std::chrono::steady_clock::now());
        const auto present_started = std::chrono::steady_clock::now();
        if (!SDL_RenderClear(renderer_.get()) ||
            !SDL_RenderTexture(renderer_.get(), texture_.get(), nullptr, nullptr)) {
            throw std::runtime_error(std::format("SDL presentation failed: {}", SDL_GetError()));
        }
        const auto render_submitted = std::chrono::steady_clock::now();
        if (inspection_ && texture_translated) {
            const auto texture_reference_started = render_submitted;
            full_reference_match =
                full_reference_match &&
                texture_matches_pixels(pixel_bytes, pixel_width, pixel_height, row_bytes);
            timings.reference_us +=
                elapsed_microseconds(texture_reference_started, std::chrono::steady_clock::now());
        }
        const auto present_resumed = std::chrono::steady_clock::now();
        update_text_input_area(frame);
        SDL_RenderPresent(renderer_.get());
        const auto present_finished = std::chrono::steady_clock::now();
        timings.present_us = elapsed_microseconds(present_started, render_submitted) +
                             elapsed_microseconds(present_resumed, present_finished);
        timings.total_us = elapsed_microseconds(frame_started, present_finished);
        rendered_scale_ = scale;
        if (editor_snapshot) {
            publish_inspection(std::move(*editor_snapshot), frame, render_diagnostics,
                               full_reference_match, timings);
        }
        frame_controller_.did_present(frame);
        presented_frame_ = std::move(frame);
    }

    void publish_inspection(EditorStateSnapshot editor_snapshot, const PresentedFrame& frame,
                            const SkiaRenderDiagnostics& diagnostics, bool full_reference_match,
                            const RenderTimingSnapshot& timings) {
        if (!inspection_) {
            return;
        }
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        const SkiaTheme& theme = presenter_.theme();
        const PresentationStyleSheet& styles = presenter_.styles();
        const PresentationMetrics& metrics = presenter_.metrics();
        const auto snapshot_rect = [](const SkiaLogicalRect& rect) {
            return LogicalPixelRectSnapshot{
                .x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height};
        };
        const FrameAnimationState& frame_animation = frame.animation_state();
        RenderAnimationSnapshot animation{
            .active = frame_animation.active,
            .scroll = frame_animation.scroll,
            .cursor = frame_animation.cursor,
            .cursor_constrained = frame_animation.cursor_constrained,
            .cursor_owner = std::string(cursor_owner_name(frame_animation.cursor_owner)),
            .scroll_progress = frame_animation.scroll_progress,
            .cursor_progress = frame_animation.cursor_progress,
            .scroll_velocity = frame_animation.scroll_velocity,
            .visual_scroll_top = frame_animation.visual_scroll_top,
            .target_scroll_top = frame_animation.target_scroll_top,
            .layers = {},
            .active_line_y = frame_animation.active_line_y,
            .cursor_rect = std::nullopt,
        };
        animation.layers.reserve(frame_animation.layers.size());
        for (const FrameScrollLayerState& layer : frame_animation.layers) {
            animation.layers.push_back({.scroll_top = layer.scroll_top,
                                        .grid_offset_y = layer.grid_offset_y,
                                        .clip_top = layer.clip_top,
                                        .clip_bottom = layer.clip_bottom});
        }
        if (frame_animation.cursor_rect) {
            animation.cursor_rect = snapshot_rect(*frame_animation.cursor_rect);
        }
        std::vector<PrimitiveRenderSnapshot> primitives;
        primitives.reserve(diagnostics.primitives.size());
        for (const SkiaPrimitiveRenderDiagnostics& primitive : diagnostics.primitives) {
            PrimitiveRenderSnapshot snapshot;
            snapshot.region_index = primitive.region_index;
            snapshot.primitive_index = primitive.primitive_index;
            snapshot.layout_bounds = snapshot_rect(primitive.layout_bounds);
            snapshot.draw_bounds_cross_region_clip = primitive.draw_bounds_cross_region_clip;
            snapshot.row_overflow = primitive.row_overflow;
            snapshot.column_overflow = primitive.column_overflow;
            if (primitive.shape_bounds) {
                snapshot.shape_bounds = snapshot_rect(*primitive.shape_bounds);
            }
            if (primitive.paint_bounds) {
                snapshot.paint_bounds = snapshot_rect(*primitive.paint_bounds);
            }
            primitives.push_back(std::move(snapshot));
        }
        const auto snapshot_text = [&](const SkiaTextLayoutDiagnostics& text) {
            TextLayoutSnapshot snapshot{
                .role = text.role,
                .byte_count = text.byte_count,
                .advance = text.advance,
                .origin = {.x = text.origin.x, .y = text.origin.y},
                .shape_bounds = std::nullopt,
            };
            if (text.shape_bounds) {
                snapshot.shape_bounds = snapshot_rect(*text.shape_bounds);
            }
            return snapshot;
        };
        std::optional<DocumentLayoutSnapshot> document_layout;
        if (diagnostics.document_layout) {
            const SkiaDocumentLayoutDiagnostics& document = *diagnostics.document_layout;
            DocumentLayoutSnapshot snapshot{
                .bounds = snapshot_rect(document.bounds),
                .cursor_row = document.cursor_row,
                .cursor_column = document.cursor_column,
                .cursor_advance = document.cursor_advance,
                .grid_cursor_x = document.grid_cursor_x,
                .cursor_rect = std::nullopt,
                .lines = {},
            };
            if (document.cursor_rect) {
                snapshot.cursor_rect = snapshot_rect(*document.cursor_rect);
            }
            snapshot.lines.reserve(document.lines.size());
            for (const SkiaDocumentLineLayoutDiagnostics& line : document.lines) {
                snapshot.lines.push_back({.row = line.row,
                                          .end_column = line.end_column,
                                          .origin_x = line.origin_x,
                                          .advance = line.advance,
                                          .run_count = line.run_count});
            }
            document_layout = std::move(snapshot);
        }
        std::optional<PopupLayoutSnapshot> popup_layout;
        if (diagnostics.popup_layout) {
            const SkiaPopupLayoutDiagnostics& popup = *diagnostics.popup_layout;
            PopupLayoutSnapshot snapshot{
                .panel_bounds = snapshot_rect(popup.panel_bounds),
                .header_bounds = snapshot_rect(popup.header_bounds),
                .horizontal_scroll = popup.horizontal_scroll,
                .input_bytes = popup.input_bytes,
                .input_cursor = popup.input_cursor,
                .cursor_advance = popup.cursor_advance,
                .unclamped_cursor_x = popup.unclamped_cursor_x,
                .cursor_clamped = popup.cursor_clamped,
                .cursor_rect = std::nullopt,
                .header_text = {},
            };
            if (popup.cursor_rect) {
                snapshot.cursor_rect = snapshot_rect(*popup.cursor_rect);
            }
            snapshot.header_text.reserve(popup.header_text.size());
            for (const SkiaTextLayoutDiagnostics& text : popup.header_text) {
                snapshot.header_text.push_back(snapshot_text(text));
            }
            popup_layout = std::move(snapshot);
        }
        std::optional<EchoLayoutSnapshot> echo_layout;
        if (diagnostics.echo_layout) {
            const SkiaEchoLayoutDiagnostics& echo = *diagnostics.echo_layout;
            EchoLayoutSnapshot snapshot{
                .bounds = snapshot_rect(echo.bounds),
                .horizontal_scroll = echo.horizontal_scroll,
                .text_bytes = echo.text_bytes,
                .cursor_byte = echo.cursor_byte,
                .cursor_advance = echo.cursor_advance,
                .unclamped_cursor_x = echo.unclamped_cursor_x,
                .cursor_clamped = echo.cursor_clamped,
                .cursor_rect = std::nullopt,
                .text = snapshot_text(echo.text),
            };
            if (echo.cursor_rect) {
                snapshot.cursor_rect = snapshot_rect(*echo.cursor_rect);
            }
            echo_layout = std::move(snapshot);
        }
        RenderDamageSnapshot render_damage{
            .full_repaint = frame.full_presentation_repaint(),
            .grid_transform_changed = frame.scene_damage().grid_transform_changed,
            .grid_translation_rows = frame.scene_damage().grid_translation_rows,
            .damaged_cells = frame.scene_damage().damaged_cells,
            .damaged_output_pixels = 0,
            .output_fraction = 0.0,
            .full_reference_match = full_reference_match,
            .rects = {},
        };
        render_damage.rects.reserve(frame.damage().size());
        for (const FrameDamageRect& rect : frame.damage()) {
            render_damage.damaged_output_pixels += static_cast<std::uint64_t>(rect.output.width) *
                                                   static_cast<std::uint64_t>(rect.output.height);
            render_damage.rects.push_back({
                .logical = snapshot_rect(rect.logical),
                .output = {.x = rect.output.x,
                           .y = rect.output.y,
                           .width = rect.output.width,
                           .height = rect.output.height},
            });
        }
        const std::uint64_t output_pixels = static_cast<std::uint64_t>(frame.output_width()) *
                                            static_cast<std::uint64_t>(frame.output_height());
        if (output_pixels != 0) {
            render_damage.output_fraction =
                static_cast<double>(render_damage.damaged_output_pixels) /
                static_cast<double>(output_pixels);
        }
        RenderStateSnapshot render{
            .video_driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "",
            .render_driver =
                SDL_GetRendererName(renderer_.get()) ? SDL_GetRendererName(renderer_.get()) : "",
            .window_width = window_width,
            .window_height = window_height,
            .output_width = frame.output_width(),
            .output_height = frame.output_height(),
            .display_scale = frame.display_scale(),
            .cell_width = presenter_.cell_width(),
            .cell_height = presenter_.cell_height(),
            .rows = rows_,
            .columns = columns_,
            .texture_format = "ARGB8888",
            .font_family = presenter_.font_family(),
            .font_size = presenter_.font_size(),
            .font_metrics = {.ascent = diagnostics.ascent,
                             .descent = diagnostics.descent,
                             .leading = diagnostics.leading,
                             .baseline_from_row_top = diagnostics.baseline_from_row_top},
            .theme = {.canvas = theme.canvas,
                      .highlight = theme.highlight,
                      .band = theme.band,
                      .selection = theme.selection,
                      .divider = theme.divider,
                      .text = theme.text,
                      .strong = theme.strong,
                      .faded = theme.faded,
                      .faint = theme.faint,
                      .salient = theme.salient,
                      .popout = theme.popout,
                      .critical = theme.critical,
                      .cursor = theme.cursor,
                      .sign_added = theme.sign_added,
                      .sign_modified = theme.sign_modified,
                      .sign_deleted = theme.sign_deleted},
            .styles = styles,
            .metrics = metrics,
            .pixel_hash = hash_pixels(),
            .animation = animation,
            .damage = std::move(render_damage),
            .timings = timings,
            .document_layout = std::move(document_layout),
            .popup_layout = std::move(popup_layout),
            .echo_layout = std::move(echo_layout),
            .primitives = std::move(primitives),
        };
        inspection_->publish(std::move(editor_snapshot), frame.scene(), std::move(render),
                             last_repaint_event_sequence_);
    }

    std::uint64_t hash_pixels() const {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint32_t pixel : pixels_) {
            hash ^= pixel;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    void update_text_input_area(const PresentedFrame& frame) {
        int window_width = 0;
        int window_height = 0;
        if (!SDL_GetWindowSize(window_.get(), &window_width, &window_height) || window_width <= 0 ||
            window_height <= 0) {
            return;
        }
        const float scale = frame.display_scale();
        const float x_factor =
            static_cast<float>(window_width) * scale / static_cast<float>(frame.output_width());
        const float y_factor =
            static_cast<float>(window_height) * scale / static_cast<float>(frame.output_height());
        const std::optional<SkiaLogicalRect>& cursor = frame.view().cursor_rect;
        if (!cursor) {
            return;
        }
        SDL_Rect area{
            .x = static_cast<int>(std::floor(cursor->x * x_factor)),
            .y = static_cast<int>(std::floor(cursor->y * y_factor)),
            .w = std::max(1, static_cast<int>(std::ceil(cursor->width * x_factor))),
            .h = std::max(1, static_cast<int>(std::ceil(cursor->height * y_factor))),
        };
        SDL_SetTextInputArea(window_.get(), &area, 0);
    }

    std::optional<ui::HitTarget> mouse_hit_target(float window_x, float window_y) const {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        if (window_width <= 0 || window_height <= 0) {
            return std::nullopt;
        }
        const float logical_x = window_x / static_cast<float>(window_width) * logical_output_width_;
        const float logical_y =
            window_y / static_cast<float>(window_height) * logical_output_height_;
        if (presented_frame_) {
            return presented_frame_->hit_test(presenter_, {.x = logical_x, .y = logical_y});
        }
        return std::nullopt;
    }

#if defined(__APPLE__)
    void queue_mac_scroll(MacScrollDelta delta) {
        if (delta.precise) {
            pending_mac_scroll_lines_ += delta.y / static_cast<float>(presenter_.cell_height());
        } else {
            pending_mac_scroll_steps_ += delta.y;
        }
        if (mac_scroll_event_queued_) {
            return;
        }
        SDL_Event event{};
        event.type = mac_scroll_event_;
        if (!SDL_PushEvent(&event)) {
            pending_mac_scroll_lines_ = 0.0F;
            pending_mac_scroll_steps_ = 0.0F;
            return;
        }
        mac_scroll_event_queued_ = true;
    }
#endif

    EditorModel& editor_;
    SkiaPresenter& presenter_;
    GuiFrameController frame_controller_;
    InspectionHub* inspection_ = nullptr;
    Uint32 background_event_ = 0;
#if defined(__APPLE__)
    Uint32 mac_scroll_event_ = 0;
    void* mac_scroll_monitor_ = nullptr;
    float pending_mac_scroll_lines_ = 0.0F;
    float pending_mac_scroll_steps_ = 0.0F;
    float last_mac_scroll_lines_ = 0.0F;
    float last_mac_scroll_steps_ = 0.0F;
    bool mac_scroll_event_queued_ = false;
#endif
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
    std::unique_ptr<SDL_Texture, TextureDeleter> texture_;
    std::unique_ptr<SDL_Texture, TextureDeleter> scroll_texture_;
    std::vector<std::uint32_t> pixels_;
    std::vector<std::uint32_t> diagnostic_pixels_;
    std::optional<PresentedFrame> presented_frame_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    float rendered_scale_ = 0.0F;
    float logical_output_width_ = 0.0F;
    float logical_output_height_ = 0.0F;
    int rows_ = 24;
    int columns_ = 80;
    int page_rows_ = 22;
    bool direct_scroll_pending_ = false;
    bool suppress_text_input_ = false;
    bool vsync_enabled_ = false;
    std::uint64_t last_repaint_event_sequence_ = 0;
};

// Renders one frame headless and writes the raw N32 (BGRA) pixels. No SDL, no
// The presenter rasterizes into an owned buffer, the reliable way to capture
// the real chrome for font-smoothing comparisons. The vendored Skia is
// built without encoders, so a tiny external step turns the dump into a PNG.
struct ScreenshotGeometry {
    float font_size = 16.0F;
    int logical_width = 0;
    int logical_height = 0;
    float scale = 1.0F;
};

int run_screenshot(const std::string& path, std::uint32_t initial_line,
                   const std::filesystem::path& output, SkiaFontSmoothing smoothing,
                   std::string font_family, ScreenshotGeometry geometry,
                   std::string_view key_notation) {
    HeadlessWakeup wakeup;
    EditorModel editor(path, std::nullopt, CppIndentStyle{}, "llvm (fallback)", initial_line,
                       {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wakeup] {
                            wakeup.notify();
                        }});
    while (editor.has_background_work()) {
        wakeup.wait();
        (void)editor.poll_background_work();
    }
    SkiaPresenter presenter(std::move(font_family), geometry.font_size, editor.presentation_theme(),
                            editor.presentation_styles(), editor.presentation_metrics(), smoothing);

    const float cell_height = static_cast<float>(presenter.cell_height());
    const float cell_width = static_cast<float>(presenter.cell_width());
    const float logical_w = static_cast<float>(geometry.logical_width);
    const float logical_h = static_cast<float>(geometry.logical_height);
    const int rows = std::max(3, static_cast<int>(std::ceil(logical_h / cell_height)));
    const int columns = std::max(20, static_cast<int>(std::ceil(logical_w / cell_width)));
    const float footer = presenter.status_bar_height() + presenter.echo_area_height();
    const float text_height = std::max(0.0F, logical_h - footer);
    const float visible_text_rows =
        std::clamp(text_height / cell_height, 1.0F, static_cast<float>(rows - 2));
    if (!key_notation.empty()) {
        const std::expected<KeySequence, KeyParseError> keys = parse_key_sequence(key_notation);
        if (!keys) {
            throw std::runtime_error(
                std::format("--screenshot-keys could not parse '{}'", key_notation));
        }
        for (const KeyStroke& key : *keys) {
            const bool handled = editor.handle_key(key, rows - 2);
            if (key.code == KeyCode::Character && key.modifiers.bits == 0 && !handled) {
                std::string utf8;
                const char32_t point = key.character;
                if (point < 0x80) {
                    utf8.push_back(static_cast<char>(point));
                } else if (point < 0x800) {
                    utf8.push_back(static_cast<char>(0xC0 | (point >> 6)));
                    utf8.push_back(static_cast<char>(0x80 | (point & 0x3F)));
                } else if (point < 0x10000) {
                    utf8.push_back(static_cast<char>(0xE0 | (point >> 12)));
                    utf8.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
                    utf8.push_back(static_cast<char>(0x80 | (point & 0x3F)));
                } else {
                    utf8.push_back(static_cast<char>(0xF0 | (point >> 18)));
                    utf8.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
                    utf8.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
                    utf8.push_back(static_cast<char>(0x80 | (point & 0x3F)));
                }
                editor.insert_text(utf8);
            }
            while (editor.has_background_work()) {
                wakeup.wait();
                (void)editor.poll_background_work();
            }
        }
    }
    editor.layout_view(rows, columns, visible_text_rows);
    ui::Scene scene = editor.compose(rows, columns, visible_text_rows);

    const int pixel_width = static_cast<int>(std::lround(logical_w * geometry.scale));
    const int pixel_height = static_cast<int>(std::lround(logical_h * geometry.scale));
    const std::size_t row_bytes = static_cast<std::size_t>(pixel_width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(pixel_width) *
                                      static_cast<std::size_t>(pixel_height));
    presenter.render(scene, pixel_width, pixel_height, pixels.data(), row_bytes, geometry.scale);

    std::ofstream out(output, std::ios::binary);
    if (!out) {
        throw std::runtime_error(std::format("cannot write screenshot {}", output.string()));
    }
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size() * sizeof(std::uint32_t)));
    std::fprintf(stdout, "%d %d\n", pixel_width, pixel_height);
    std::fprintf(stderr, "cind-gui screenshot: %s (%dx%d BGRA)\n", output.c_str(), pixel_width,
                 pixel_height);
    return 0;
}

int run_editor(const std::string& path, std::uint32_t initial_line,
               const std::optional<std::filesystem::path>& inspector_socket,
               SkiaFontSmoothing smoothing, std::string font_family, float font_size) {
    SdlRuntime runtime;
    const Uint32 background_event = SDL_RegisterEvents(1);
    if (background_event == 0) {
        throw std::runtime_error(
            std::format("SDL user event allocation failed: {}", SDL_GetError()));
    }
    EditorPlatformServices platform_services{
        .write_clipboard = [](std::string_view text) -> std::expected<void, std::string> {
            const std::string owned(text);
            if (!SDL_SetClipboardText(owned.c_str())) {
                return std::unexpected(std::format("SDL_SetClipboardText: {}", SDL_GetError()));
            }
            return {};
        },
        .read_clipboard = []() -> std::expected<std::string, std::string> {
            (void)SDL_ClearError();
            const std::unique_ptr<char, SdlMemoryDeleter> text(SDL_GetClipboardText());
            const std::string error(SDL_GetError());
            if (text == nullptr || !error.empty()) {
                return std::unexpected(std::format("SDL_GetClipboardText: {}", error));
            }
            return std::string(text.get());
        },
        .wake_event_loop =
            [background_event] {
                SDL_Event event{};
                event.type = background_event;
                (void)SDL_PushEvent(&event);
            }};
    EditorModel editor(path, std::nullopt, CppIndentStyle{}, "llvm (fallback)", initial_line,
                       std::move(platform_services), discover_user_init_file());
    SkiaPresenter presenter(std::move(font_family), font_size, editor.presentation_theme(),
                            editor.presentation_styles(), editor.presentation_metrics(), smoothing);
    std::unique_ptr<InspectionHub> inspection;
    std::unique_ptr<InspectorServer> inspector;
    if (inspector_socket) {
        inspection = std::make_unique<InspectionHub>();
        inspector = std::make_unique<InspectorServer>(*inspection, *inspector_socket);
        presenter.set_show_debug_status(true);
        std::fprintf(stderr, "cind-gui inspector: %s\n", inspector->socket_path().c_str());
    }
    SdlWindow window(editor, presenter, inspection.get(), background_event);
    window.run();
    return 0;
}

} // namespace

} // namespace cind::gui

int main(int argc, char** argv) {
    std::uint32_t initial_line = 0;
    bool inspect = false;
    std::optional<std::filesystem::path> inspector_socket;
    std::optional<std::string> file;
    std::optional<std::filesystem::path> screenshot;
    std::string screenshot_keys;
    cind::gui::SkiaFontSmoothing smoothing = cind::gui::SkiaFontSmoothing::Smooth;
    std::string_view font_family = "monospace";
    float font_size = 16.0F;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--inspect") {
                inspect = true;
            } else if (argument == "--help" || argument == "-h") {
                std::fprintf(stderr, "usage: cind-gui [--inspect] [--inspect-socket PATH] "
                                     "[--font-family FAMILY] [--font-size SIZE] "
                                     "[--font-smoothing smooth|crisp|sharp|lcd] [+LINE] <file>\n");
                return 0;
            } else if (argument == "--inspect-socket") {
                if (++index >= argc) {
                    throw std::runtime_error("--inspect-socket requires a path");
                }
                inspect = true;
                inspector_socket = std::filesystem::absolute(argv[index]);
            } else if (argument == "--font-smoothing") {
                if (++index >= argc) {
                    throw std::runtime_error("--font-smoothing requires a mode");
                }
                smoothing = cind::gui::parse_font_smoothing(argv[index]);
            } else if (argument == "--font-family") {
                if (++index >= argc) {
                    throw std::runtime_error("--font-family requires a family");
                }
                font_family = argv[index];
                if (font_family.empty()) {
                    throw std::runtime_error("--font-family must not be empty");
                }
            } else if (argument == "--screenshot") {
                if (++index >= argc) {
                    throw std::runtime_error("--screenshot requires a path");
                }
                screenshot = std::filesystem::absolute(argv[index]);
            } else if (argument == "--screenshot-keys") {
                if (++index >= argc) {
                    throw std::runtime_error("--screenshot-keys requires a key sequence");
                }
                screenshot_keys = argv[index];
            } else if (argument == "--font-size") {
                if (++index >= argc) {
                    throw std::runtime_error("--font-size requires a value");
                }
                font_size = std::stof(argv[index]);
                if (!(font_size > 0.0F)) {
                    throw std::runtime_error("--font-size must be positive");
                }
            } else if (argument.starts_with('+')) {
                if (argument.size() == 1 || initial_line != 0) {
                    throw std::runtime_error("invalid +LINE argument");
                }
                char* end = nullptr;
                errno = 0;
                const unsigned long parsed = std::strtoul(argument.data() + 1, &end, 10);
                if (errno != 0 || !end || *end != '\0' || parsed == 0 ||
                    parsed > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error("invalid +LINE argument");
                }
                initial_line = static_cast<std::uint32_t>(parsed);
            } else if (!file) {
                file = std::string(argument);
            } else {
                throw std::runtime_error("more than one file was provided");
            }
        }
        if (!file) {
            std::fprintf(stderr, "usage: cind-gui [--inspect] [--inspect-socket PATH] "
                                 "[--font-family FAMILY] [--font-size SIZE] "
                                 "[--font-smoothing smooth|crisp|sharp|lcd] [+LINE] <file>\n");
            return 2;
        }
        if (screenshot) {
            return cind::gui::run_screenshot(*file, initial_line, *screenshot, smoothing,
                                             std::string(font_family),
                                             {.font_size = font_size,
                                              .logical_width = 900,
                                              .logical_height = 600,
                                              .scale = 1.5F},
                                             screenshot_keys);
        }
        if (inspect && !inspector_socket) {
            inspector_socket =
                cind::gui::default_inspector_socket_path(static_cast<int>(::getpid()));
        }
        return cind::gui::run_editor(*file, initial_line, inspector_socket, smoothing,
                                     std::string(font_family), font_size);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cind-gui: %s\n", error.what());
        return 1;
    }
}
#include <condition_variable>
