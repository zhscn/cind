#include "cli/style_loader.hpp"
#include "gui/editor_model.hpp"
#include "gui/inspect_server.hpp"
#include "gui/inspection.hpp"
#include "gui/motion.hpp"
#include "gui/skia_presenter.hpp"
#include "ui/scene_layout.hpp"

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
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cind::gui {

namespace {

struct PresentationDamageRect {
    SkiaLogicalRect logical;
    OutputPixelRectSnapshot output;
};

struct OutputSize {
    int width = 0;
    int height = 0;
};

using AnimationClock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds cursor_animation_duration{70};
constexpr float scroll_spring_frequency = 32.0F;
constexpr float scroll_position_tolerance = 0.001F;
constexpr float scroll_velocity_tolerance = 0.01F;
constexpr float wheel_lines_per_step = 3.0F;

float animation_progress(AnimationClock::time_point started, std::chrono::milliseconds duration,
                         AnimationClock::time_point now) {
    const auto elapsed = std::chrono::duration<float>(now - started);
    const auto total = std::chrono::duration<float>(duration);
    return std::clamp(elapsed.count() / total.count(), 0.0F, 1.0F);
}

float ease_out_cubic(float progress) {
    const float remaining = 1.0F - progress;
    return 1.0F - remaining * remaining * remaining;
}

float interpolate(float from, float to, float progress) {
    return from + (to - from) * progress;
}

SkiaLogicalPoint interpolate(SkiaLogicalPoint from, SkiaLogicalPoint to, float progress) {
    return {.x = interpolate(from.x, to.x, progress), .y = interpolate(from.y, to.y, progress)};
}

bool same_point(SkiaLogicalPoint left, SkiaLogicalPoint right) {
    constexpr float tolerance = 0.01F;
    return std::abs(left.x - right.x) < tolerance && std::abs(left.y - right.y) < tolerance;
}

bool touches_or_intersects(const OutputPixelRectSnapshot& left,
                           const OutputPixelRectSnapshot& right) {
    return left.x <= right.x + right.width && right.x <= left.x + left.width &&
           left.y <= right.y + right.height && right.y <= left.y + left.height;
}

OutputPixelRectSnapshot joined(const OutputPixelRectSnapshot& left,
                               const OutputPixelRectSnapshot& right) {
    const int first_x = std::min(left.x, right.x);
    const int first_y = std::min(left.y, right.y);
    const int last_x = std::max(left.x + left.width, right.x + right.width);
    const int last_y = std::max(left.y + left.height, right.y + right.height);
    return {.x = first_x, .y = first_y, .width = last_x - first_x, .height = last_y - first_y};
}

SkiaLogicalRect joined(const SkiaLogicalRect& left, const SkiaLogicalRect& right) {
    const float first_x = std::min(left.x, right.x);
    const float first_y = std::min(left.y, right.y);
    const float last_x = std::max(left.x + left.width, right.x + right.width);
    const float last_y = std::max(left.y + left.height, right.y + right.height);
    return {.x = first_x, .y = first_y, .width = last_x - first_x, .height = last_y - first_y};
}

std::vector<PresentationDamageRect>
presentation_damage(std::span<const SkiaLogicalRect> logical_rects, int pixel_width,
                    int pixel_height, float scale) {
    std::vector<PresentationDamageRect> result;
    for (const SkiaLogicalRect& logical : logical_rects) {
        const int first_x =
            std::clamp(static_cast<int>(std::floor(logical.x * scale)), 0, pixel_width);
        const int first_y =
            std::clamp(static_cast<int>(std::floor(logical.y * scale)), 0, pixel_height);
        const int last_x = std::clamp(
            static_cast<int>(std::ceil((logical.x + logical.width) * scale)), 0, pixel_width);
        const int last_y = std::clamp(
            static_cast<int>(std::ceil((logical.y + logical.height) * scale)), 0, pixel_height);
        if (last_x <= first_x || last_y <= first_y) {
            continue;
        }

        PresentationDamageRect next{
            .logical = logical,
            .output = {.x = first_x,
                       .y = first_y,
                       .width = last_x - first_x,
                       .height = last_y - first_y},
        };
        for (std::size_t index = 0; index < result.size();) {
            if (!touches_or_intersects(result[index].output, next.output)) {
                ++index;
                continue;
            }
            next.logical = joined(result[index].logical, next.logical);
            next.output = joined(result[index].output, next.output);
            result.erase(result.begin() + static_cast<std::ptrdiff_t>(index));
            index = 0;
        }
        result.push_back(next);
    }
    return result;
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
    case SDL_SCANCODE_5:
        return punctuation(U'5', U'%');
    default:
        return std::nullopt;
    }
}

class SdlRuntime {
public:
    SdlRuntime() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(
                std::format("SDL video initialization failed: {}", SDL_GetError()));
        }
        initialized_ = true;
        const char* driver = SDL_GetCurrentVideoDriver();
        if (!driver || std::string_view(driver) != "wayland") {
            SDL_Quit();
            initialized_ = false;
            throw std::runtime_error(
                std::format("SDL selected '{}' instead of Wayland", driver ? driver : "none"));
        }
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

struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const { SDL_DestroyRenderer(renderer); }
};

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};

class SdlWindow {
public:
    SdlWindow(EditorModel& editor, SkiaPresenter& presenter, InspectionHub* inspection)
        : editor_(editor), presenter_(presenter), inspection_(inspection) {
        constexpr SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        window_.reset(SDL_CreateWindow("cind · Skia · Wayland", 1100, 760, flags));
        if (!window_) {
            throw std::runtime_error(std::format("SDL window creation failed: {}", SDL_GetError()));
        }
        SDL_SetWindowMinimumSize(window_.get(), presenter_.cell_width() * 40,
                                 presenter_.cell_height() * 6);
        renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
        if (!renderer_) {
            throw std::runtime_error(
                std::format("SDL renderer creation failed: {}", SDL_GetError()));
        }
        SDL_SetRenderVSync(renderer_.get(), 1);
        if (!SDL_StartTextInput(window_.get())) {
            throw std::runtime_error(std::format("SDL text input failed: {}", SDL_GetError()));
        }
    }

    void run() {
        paint();
        while (!editor_.should_quit()) {
            SDL_Event event{};
            const bool asynchronous = editor_.has_background_work() || animations_active();
            const bool received =
                asynchronous ? SDL_WaitEventTimeout(&event, 16) : SDL_WaitEvent(&event);
            if (!received && !asynchronous) {
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
            repaint = animations_active() || repaint;
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

    struct ScrollAnimation {
        ui::Scene source;
        float source_scroll_top = 0.0F;
        SpringState motion;
        float target_scroll_top = 0.0F;
        float initial_distance = 0.0F;
        AnimationClock::time_point sampled_at;
    };

    struct CursorAnimation {
        SkiaLogicalPoint from;
        SkiaLogicalPoint target;
        AnimationClock::time_point started;
    };

    struct AnimationPresentation {
        bool active = false;
        bool scroll_finished = false;
        bool cursor_finished = false;
        SkiaAnimationFrame frame;
        RenderAnimationSnapshot snapshot;
    };

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
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            editor_.request_quit();
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
            const bool continued_sequence = editor_.has_pending_key_sequence();
            const std::optional<KeyStroke> key = editor_key(event.key.scancode, event.key.mod);
            const bool handled = key && editor_.handle_key(*key, page_rows_);
            constexpr SDL_Keymod command_modifiers = static_cast<SDL_Keymod>(
                SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_GUI);
            suppress_text_input_ = handled && continued_sequence && key &&
                                   key->code == KeyCode::Character &&
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
                editor_.click({.row = mouse_cell_row(event.button.y),
                               .column = mouse_cell_column(event.button.x)});
                return {true, true};
            }
            return {};
        case SDL_EVENT_MOUSE_WHEEL: {
            float amount = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                amount = -amount;
            }
            wheel_scroll_accumulator_ -= amount * wheel_lines_per_step;
            const int lines = static_cast<int>(wheel_scroll_accumulator_);
            if (lines != 0) {
                editor_.scroll_lines(lines);
                wheel_scroll_accumulator_ -= static_cast<float>(lines);
            }
            const bool handled = amount != 0.0F;
            return {handled, lines != 0};
        }
        default:
            return {};
        }
    }

    static std::string event_type(const SDL_Event& event) {
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
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            return std::format(
                "scancode={} name={} modifiers={}", static_cast<int>(event.key.scancode),
                SDL_GetScancodeName(event.key.scancode), static_cast<unsigned int>(event.key.mod));
        case SDL_EVENT_TEXT_INPUT:
            return std::format("text={}", event.text.text ? event.text.text : "");
        case SDL_EVENT_TEXT_EDITING:
            return std::format("preedit={}", event.edit.text ? event.edit.text : "");
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            return std::format("button={} window=({}, {}) cell=({}, {})",
                               static_cast<int>(event.button.button), event.button.x,
                               event.button.y, mouse_cell_row(event.button.y),
                               mouse_cell_column(event.button.x));
        case SDL_EVENT_MOUSE_WHEEL:
            return std::format("delta=({}, {}) direction={}", event.wheel.x, event.wheel.y,
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
        texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, width, height));
        if (!texture_) {
            throw std::runtime_error(
                std::format("SDL texture creation failed: {}", SDL_GetError()));
        }
        texture_width_ = width;
        texture_height_ = height;
    }

    float display_scale() const {
        const float scale = SDL_GetWindowDisplayScale(window_.get());
        return scale > 0.0F ? scale : 1.0F;
    }

    bool animations_active() const {
        return scroll_animation_.has_value() || cursor_animation_.has_value();
    }

    SpringState scroll_state(const ScrollAnimation& animation,
                             AnimationClock::time_point now) const {
        const float elapsed = std::chrono::duration<float>(now - animation.sampled_at).count();
        return advance_critical_spring(animation.motion,
                                       {.target = animation.target_scroll_top,
                                        .angular_frequency = scroll_spring_frequency,
                                        .elapsed_seconds = elapsed});
    }

    SkiaLogicalPoint cursor_position(const CursorAnimation& animation,
                                     AnimationClock::time_point now) const {
        const float progress =
            ease_out_cubic(animation_progress(animation.started, cursor_animation_duration, now));
        return interpolate(animation.from, animation.target, progress);
    }

    std::optional<SkiaLogicalPoint> scene_cursor_position(const ui::Scene& scene) const {
        if (!scene.cursor_visible || !vertical_layout_) {
            return std::nullopt;
        }
        return SkiaLogicalPoint{
            .x = static_cast<float>(std::max(0, scene.cursor_col - 1) * presenter_.cell_width()),
            .y = vertical_layout_->row_top(std::max(0, scene.cursor_row - 1)),
        };
    }

    void update_animation_targets(const ui::Scene& scene, float scroll_top, bool geometry_changed,
                                  AnimationClock::time_point now) {
        const std::optional<SkiaLogicalPoint> target_cursor = scene_cursor_position(scene);
        if (geometry_changed || !last_scene_ || !last_scroll_top_) {
            scroll_animation_.reset();
            cursor_animation_.reset();
        } else if (std::abs(*last_scroll_top_ - scroll_top) > 0.0001F) {
            const float line_delta = scroll_top - *last_scroll_top_;
            if (std::abs(line_delta) <= 4) {
                const SpringState motion = scroll_animation_
                                               ? scroll_state(*scroll_animation_, now)
                                               : SpringState{.position = *last_scroll_top_};
                ui::Scene source_scene = *last_scene_;
                float source_scroll_top = *last_scroll_top_;
                if (scroll_animation_) {
                    source_scene = scroll_animation_->source;
                    source_scroll_top = scroll_animation_->source_scroll_top;
                }
                scroll_animation_ = ScrollAnimation{
                    .source = std::move(source_scene),
                    .source_scroll_top = source_scroll_top,
                    .motion = motion,
                    .target_scroll_top = scroll_top,
                    .initial_distance = std::abs(scroll_top - motion.position),
                    .sampled_at = now,
                };
            } else {
                scroll_animation_.reset();
            }
            cursor_animation_.reset();
        } else if (!scroll_animation_ && target_cursor && last_cursor_target_ &&
                   !same_point(*last_cursor_target_, *target_cursor)) {
            const SkiaLogicalPoint from =
                cursor_animation_ ? cursor_position(*cursor_animation_, now) : *last_cursor_target_;
            const float maximum_x_distance = static_cast<float>(presenter_.cell_width() * 4);
            const float maximum_y_distance = static_cast<float>(presenter_.cell_height() * 2);
            if (std::abs(target_cursor->x - from.x) <= maximum_x_distance &&
                std::abs(target_cursor->y - from.y) <= maximum_y_distance) {
                cursor_animation_ =
                    CursorAnimation{.from = from, .target = *target_cursor, .started = now};
            } else {
                cursor_animation_.reset();
            }
        } else if (!target_cursor) {
            cursor_animation_.reset();
        }

        last_scene_ = scene;
        last_scroll_top_ = scroll_top;
        last_cursor_target_ = target_cursor;
    }

    AnimationPresentation animation_presentation(AnimationClock::time_point now) const {
        AnimationPresentation presentation;
        if (scroll_animation_) {
            const SpringState state = scroll_state(*scroll_animation_, now);
            const bool at_rest =
                spring_at_rest(state, {.target = scroll_animation_->target_scroll_top,
                                       .position_tolerance = scroll_position_tolerance,
                                       .velocity_tolerance = scroll_velocity_tolerance});
            const float visual_top =
                at_rest ? scroll_animation_->target_scroll_top : state.position;
            presentation.active = true;
            presentation.scroll_finished = at_rest;
            presentation.frame.scroll_source = &scroll_animation_->source;
            presentation.frame.source_grid_offset_y =
                (scroll_animation_->source_scroll_top - visual_top) *
                static_cast<float>(presenter_.cell_height());
            presentation.frame.target_grid_offset_y =
                (scroll_animation_->target_scroll_top - visual_top) *
                static_cast<float>(presenter_.cell_height());
            if (!presentation.scroll_finished) {
                presentation.snapshot.active = true;
                presentation.snapshot.scroll = true;
                presentation.snapshot.scroll_progress = std::clamp(
                    1.0F - std::abs(state.position - scroll_animation_->target_scroll_top) /
                               std::max(scroll_animation_->initial_distance,
                                        scroll_position_tolerance),
                    0.0F, 1.0F);
                presentation.snapshot.scroll_velocity = state.velocity;
                presentation.snapshot.source_grid_offset_y =
                    presentation.frame.source_grid_offset_y;
                presentation.snapshot.target_grid_offset_y =
                    presentation.frame.target_grid_offset_y;
            }
        }
        if (cursor_animation_) {
            const float linear_progress =
                animation_progress(cursor_animation_->started, cursor_animation_duration, now);
            const SkiaLogicalPoint position = cursor_position(*cursor_animation_, now);
            presentation.active = true;
            presentation.cursor_finished = linear_progress >= 1.0F;
            presentation.frame.cursor_position = position;
            if (!presentation.cursor_finished) {
                presentation.snapshot.active = true;
                presentation.snapshot.cursor = true;
                presentation.snapshot.cursor_progress = ease_out_cubic(linear_progress);
                presentation.snapshot.cursor_rect = LogicalPixelRectSnapshot{
                    .x = position.x,
                    .y = position.y,
                    .width = 2.0F,
                    .height = static_cast<float>(presenter_.cell_height()),
                };
            }
        }
        return presentation;
    }

    void finish_animation_frame(const AnimationPresentation& presentation) {
        if (presentation.scroll_finished) {
            scroll_animation_.reset();
        }
        if (presentation.cursor_finished) {
            cursor_animation_.reset();
        }
    }

    void paint() {
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
        const float text_height = std::max(0.0F, logical_output_height_ - 2.0F * cell_height);
        const float visible_text_rows =
            std::clamp(text_height / cell_height, 1.0F, static_cast<float>(rows_ - 2));
        page_rows_ = std::clamp(static_cast<int>(std::floor(text_height / cell_height + 0.0001F)),
                                1, rows_ - 2);
        editor_.set_frame_rows(rows_);
        ui::Scene scene = editor_.compose(rows_, columns_, visible_text_rows);
        vertical_layout_.emplace(
            scene,
            ui::SceneVerticalMetrics{.cell_height = static_cast<float>(presenter_.cell_height()),
                                     .viewport_height = logical_output_height_});

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

        const AnimationClock::time_point now = AnimationClock::now();
        EditorStateSnapshot editor_snapshot = editor_.inspect();
        const float scroll_top = static_cast<float>(editor_snapshot.viewport.top_line) +
                                 editor_snapshot.viewport.top_line_offset;
        update_animation_targets(scene, scroll_top, geometry_changed, now);
        const AnimationPresentation animation = animation_presentation(now);
        const ui::SceneDamage scene_damage = damage_tracker_.update(scene, geometry_changed);
        std::vector<SkiaLogicalRect> logical_damage;
        if (animation.active) {
            logical_damage.push_back({.x = 0.0F,
                                      .y = 0.0F,
                                      .width = logical_output_width_,
                                      .height = logical_output_height_});
        } else {
            logical_damage = presenter_.damage_rects(scene, scene_damage, logical_output_width_,
                                                     logical_output_height_);
        }
        const std::vector<PresentationDamageRect> damage =
            presentation_damage(logical_damage, pixel_width, pixel_height, scale);
        logical_damage.clear();
        logical_damage.reserve(damage.size());
        for (const PresentationDamageRect& rect : damage) {
            logical_damage.push_back(rect.logical);
        }

        SkiaRenderDiagnostics render_diagnostics;
        if (animation.active) {
            presenter_.render_animated(scene, animation.frame, pixel_width, pixel_height,
                                       pixels_.data(), row_bytes, scale,
                                       inspection_ ? &render_diagnostics : nullptr);
        } else if (scene_damage.full_repaint) {
            presenter_.render(scene, pixel_width, pixel_height, pixels_.data(), row_bytes, scale,
                              inspection_ ? &render_diagnostics : nullptr);
        } else if (!logical_damage.empty()) {
            presenter_.render_damage(scene, pixel_width, pixel_height, pixels_.data(), row_bytes,
                                     logical_damage, scale);
        }

        bool full_reference_match = true;
        if (inspection_ && animation.active) {
            diagnostic_pixels_.resize(pixel_count);
            presenter_.render_animated(scene, animation.frame, pixel_width, pixel_height,
                                       diagnostic_pixels_.data(), row_bytes, scale);
            full_reference_match = diagnostic_pixels_ == pixels_;
        } else if (inspection_ && !scene_damage.full_repaint) {
            diagnostic_pixels_.resize(pixel_count);
            presenter_.render(scene, pixel_width, pixel_height, diagnostic_pixels_.data(),
                              row_bytes, scale, &render_diagnostics);
            full_reference_match = diagnostic_pixels_ == pixels_;
        }

        ensure_texture(pixel_width, pixel_height);
        const auto* pixel_bytes = reinterpret_cast<const std::byte*>(pixels_.data());
        for (const PresentationDamageRect& rect : damage) {
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
        }
        if (!SDL_RenderClear(renderer_.get()) ||
            !SDL_RenderTexture(renderer_.get(), texture_.get(), nullptr, nullptr)) {
            throw std::runtime_error(std::format("SDL presentation failed: {}", SDL_GetError()));
        }
        update_text_input_area(scene, {.width = pixel_width, .height = pixel_height}, scale);
        SDL_RenderPresent(renderer_.get());
        rendered_scale_ = scale;
        publish_inspection(std::move(editor_snapshot), std::move(scene), pixel_width, pixel_height,
                           scale, render_diagnostics, scene_damage,
                           scene_damage.full_repaint || animation.active, animation.snapshot,
                           damage, full_reference_match);
        finish_animation_frame(animation);
    }

    void publish_inspection(EditorStateSnapshot editor_snapshot, ui::Scene scene, int pixel_width,
                            int pixel_height, float scale, const SkiaRenderDiagnostics& diagnostics,
                            const ui::SceneDamage& scene_damage, bool full_presentation_repaint,
                            const RenderAnimationSnapshot& animation,
                            const std::vector<PresentationDamageRect>& damage,
                            bool full_reference_match) {
        if (!inspection_) {
            return;
        }
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        const SkiaTheme& theme = presenter_.theme();
        const auto snapshot_rect = [](const SkiaLogicalRect& rect) {
            return LogicalPixelRectSnapshot{
                .x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height};
        };
        std::vector<PrimitiveRenderSnapshot> primitives;
        primitives.reserve(diagnostics.primitives.size());
        for (const SkiaPrimitiveRenderDiagnostics& primitive : diagnostics.primitives) {
            PrimitiveRenderSnapshot snapshot;
            snapshot.region_index = primitive.region_index;
            snapshot.primitive_index = primitive.primitive_index;
            snapshot.cell_bounds = snapshot_rect(primitive.cell_bounds);
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
        RenderDamageSnapshot render_damage{
            .full_repaint = full_presentation_repaint,
            .damaged_cells = scene_damage.damaged_cells,
            .damaged_output_pixels = 0,
            .output_fraction = 0.0,
            .full_reference_match = full_reference_match,
            .rects = {},
        };
        render_damage.rects.reserve(damage.size());
        for (const PresentationDamageRect& rect : damage) {
            render_damage.damaged_output_pixels += static_cast<std::uint64_t>(rect.output.width) *
                                                   static_cast<std::uint64_t>(rect.output.height);
            render_damage.rects.push_back(
                {.logical = snapshot_rect(rect.logical), .output = rect.output});
        }
        const std::uint64_t output_pixels =
            static_cast<std::uint64_t>(pixel_width) * static_cast<std::uint64_t>(pixel_height);
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
            .output_width = pixel_width,
            .output_height = pixel_height,
            .display_scale = scale,
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
            .theme = {.background = theme.background,
                      .gutter_background = theme.gutter_background,
                      .status_background = theme.status_background,
                      .echo_background = theme.echo_background,
                      .selection_background = theme.selection_background,
                      .cursor = theme.cursor,
                      .sign_added = theme.sign_added,
                      .sign_modified = theme.sign_modified,
                      .sign_deleted = theme.sign_deleted},
            .pixel_hash = hash_pixels(),
            .animation = animation,
            .damage = std::move(render_damage),
            .primitives = std::move(primitives),
        };
        inspection_->publish(std::move(editor_snapshot), std::move(scene), std::move(render),
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

    void update_text_input_area(const ui::Scene& scene, OutputSize output, float scale) {
        if (!scene.cursor_visible) {
            return;
        }
        int window_width = 0;
        int window_height = 0;
        if (!SDL_GetWindowSize(window_.get(), &window_width, &window_height) || window_width <= 0 ||
            window_height <= 0) {
            return;
        }
        const float x_factor =
            static_cast<float>(window_width) * scale / static_cast<float>(output.width);
        const float y_factor =
            static_cast<float>(window_height) * scale / static_cast<float>(output.height);
        const ui::SceneVerticalLayout vertical_layout(
            scene, {.cell_height = static_cast<float>(presenter_.cell_height()),
                    .viewport_height = static_cast<float>(output.height) / scale});
        const int cursor_row = std::max(0, scene.cursor_row - 1);
        const float cursor_top = vertical_layout.row_top(cursor_row);
        float cursor_bottom = std::min(static_cast<float>(output.height) / scale,
                                       cursor_top + static_cast<float>(presenter_.cell_height()));
        if (vertical_layout.bottom_anchor_row() &&
            cursor_row < *vertical_layout.bottom_anchor_row()) {
            cursor_bottom = std::min(cursor_bottom, vertical_layout.grid_clip_bottom());
        }
        SDL_Rect area{
            .x = static_cast<int>(std::floor(static_cast<float>(scene.cursor_col - 1) *
                                             static_cast<float>(presenter_.cell_width()) *
                                             x_factor)),
            .y = static_cast<int>(std::floor(cursor_top * y_factor)),
            .w = std::max(1, static_cast<int>(std::ceil(
                                 static_cast<float>(presenter_.cell_width()) * x_factor))),
            .h = std::max(1, static_cast<int>(
                                 std::ceil(std::max(0.0F, cursor_bottom - cursor_top) * y_factor))),
        };
        SDL_SetTextInputArea(window_.get(), &area, 0);
    }

    int mouse_cell_column(float window_x) const {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        if (window_width <= 0) {
            return 0;
        }
        const float logical_x = window_x / static_cast<float>(window_width) * logical_output_width_;
        return std::clamp(
            static_cast<int>(std::floor(logical_x / static_cast<float>(presenter_.cell_width()))),
            0, columns_ - 1);
    }

    int mouse_cell_row(float window_y) const {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        if (window_height <= 0) {
            return 0;
        }
        const float logical_y =
            window_y / static_cast<float>(window_height) * logical_output_height_;
        return vertical_layout_ ? vertical_layout_->row_at(logical_y) : 0;
    }

    EditorModel& editor_;
    SkiaPresenter& presenter_;
    InspectionHub* inspection_ = nullptr;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
    std::unique_ptr<SDL_Texture, TextureDeleter> texture_;
    std::vector<std::uint32_t> pixels_;
    std::vector<std::uint32_t> diagnostic_pixels_;
    ui::SceneDamageTracker damage_tracker_;
    std::optional<ui::SceneVerticalLayout> vertical_layout_;
    std::optional<ScrollAnimation> scroll_animation_;
    std::optional<CursorAnimation> cursor_animation_;
    std::optional<ui::Scene> last_scene_;
    std::optional<float> last_scroll_top_;
    std::optional<SkiaLogicalPoint> last_cursor_target_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    float rendered_scale_ = 0.0F;
    float logical_output_width_ = 0.0F;
    float logical_output_height_ = 0.0F;
    int rows_ = 24;
    int columns_ = 80;
    int page_rows_ = 22;
    float wheel_scroll_accumulator_ = 0.0F;
    bool suppress_text_input_ = false;
    std::uint64_t last_repaint_event_sequence_ = 0;
};

int run_editor(const std::string& path, std::uint32_t initial_line,
               const std::optional<std::filesystem::path>& inspector_socket) {
    std::string initial;
    if (std::filesystem::exists(path)) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(std::format("cannot open {}", path));
        }
        std::stringstream contents;
        contents << input.rdbuf();
        initial = contents.str();
    }

    CppIndentStyle style;
    std::string style_origin = "llvm (fallback)";
    if (auto loaded = load_clang_format_style(std::filesystem::absolute(path).parent_path())) {
        style = loaded->style;
        style_origin = loaded->config_path.filename().string();
    }

    EditorModel editor(path, std::move(initial), style, std::move(style_origin), initial_line);
    SkiaPresenter presenter;
    SdlRuntime runtime;
    std::unique_ptr<InspectionHub> inspection;
    std::unique_ptr<InspectorServer> inspector;
    if (inspector_socket) {
        inspection = std::make_unique<InspectionHub>();
        inspector = std::make_unique<InspectorServer>(*inspection, *inspector_socket);
        std::fprintf(stderr, "cind-gui inspector: %s\n", inspector->socket_path().c_str());
    }
    SdlWindow window(editor, presenter, inspection.get());
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

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--inspect") {
                inspect = true;
            } else if (argument == "--help" || argument == "-h") {
                std::fprintf(stderr, "usage: cind-gui [--inspect] [--inspect-socket PATH] [+LINE] "
                                     "<file>\n");
                return 0;
            } else if (argument == "--inspect-socket") {
                if (++index >= argc) {
                    throw std::runtime_error("--inspect-socket requires a path");
                }
                inspect = true;
                inspector_socket = std::filesystem::absolute(argv[index]);
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
            std::fprintf(stderr,
                         "usage: cind-gui [--inspect] [--inspect-socket PATH] [+LINE] <file>\n");
            return 2;
        }
        if (inspect && !inspector_socket) {
            inspector_socket =
                cind::gui::default_inspector_socket_path(static_cast<int>(::getpid()));
        }
        return cind::gui::run_editor(*file, initial_line, inspector_socket);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cind-gui: %s\n", error.what());
        return 1;
    }
}
