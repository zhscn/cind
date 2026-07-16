#include "cli/style_loader.hpp"
#include "gui/editor_model.hpp"
#include "gui/inspect_server.hpp"
#include "gui/inspection.hpp"
#include "gui/motion.hpp"
#include "gui/scroll_timeline.hpp"
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
#include <expected>
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

struct SdlMemoryDeleter {
    void operator()(void* memory) const { SDL_free(memory); }
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
        vsync_enabled_ = SDL_SetRenderVSync(renderer_.get(), 1);
        if (!SDL_StartTextInput(window_.get())) {
            throw std::runtime_error(std::format("SDL text input failed: {}", SDL_GetError()));
        }
    }

    void run() {
        paint();
        while (!editor_.should_quit()) {
            SDL_Event event{};
            const bool animating = animations_active();
            const bool asynchronous = editor_.has_background_work() || animating;
            bool received = false;
            if (animating && vsync_enabled_) {
                received = SDL_PollEvent(&event);
            } else if (asynchronous) {
                received = SDL_WaitEventTimeout(&event, animating ? 1 : 16);
            } else {
                received = SDL_WaitEvent(&event);
            }
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

    struct AnimationContext {
        std::uint32_t window_slot = 0;
        std::uint32_t window_generation = 0;
        std::uint32_t view_slot = 0;
        std::uint32_t view_generation = 0;
        std::uint32_t buffer_slot = 0;
        std::uint32_t buffer_generation = 0;
        RevisionId revision = 0;

        bool operator==(const AnimationContext&) const = default;
    };

    struct ScrollAnimation {
        ScrollSceneTimeline timeline;
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

    static AnimationContext animation_context(const EditorStateSnapshot& editor) {
        for (const OpenWindowStateSnapshot& window : editor.windows) {
            if (!window.active) {
                continue;
            }
            return {.window_slot = window.window_slot,
                    .window_generation = window.window_generation,
                    .view_slot = window.view_slot,
                    .view_generation = window.view_generation,
                    .buffer_slot = window.buffer_slot,
                    .buffer_generation = window.buffer_generation,
                    .revision = editor.revision};
        }
        return {.window_slot = editor.active_window_slot,
                .window_generation = editor.active_window_generation,
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
                editor_.click(mouse_cell_point(event.button.x, event.button.y));
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
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const ui::CellPoint point = mouse_cell_point(event.button.x, event.button.y);
            return std::format("button={} window=({}, {}) cell=({}, {})",
                               static_cast<int>(event.button.button), event.button.x,
                               event.button.y, point.row, point.column);
        }
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

    std::optional<SkiaLogicalPoint> scene_cursor_position(const SkiaFrameLayout& layout) const {
        if (!vertical_layout_) {
            return std::nullopt;
        }
        const std::optional<SkiaLogicalRect> cursor = presenter_.cursor_rect(layout);
        return cursor ? std::optional<SkiaLogicalPoint>(
                            SkiaLogicalPoint{.x = cursor->x, .y = cursor->y})
                      : std::nullopt;
    }

    bool scene_cursor_uses_grid_offset(const ui::Scene& scene) const {
        if (const ui::Region* popup = scene.find(ui::RegionRole::Popup);
            popup && popup->popup && popup->popup->input) {
            return false;
        }
        const int cursor_row = scene.cursor_row - 1;
        const int cursor_col = scene.cursor_col - 1;
        for (const ui::Region& region : scene.regions) {
            const ui::Rect& rect = region.rect;
            if (cursor_row >= rect.row && cursor_row < rect.row + rect.rows &&
                cursor_col >= rect.col && cursor_col < rect.col + rect.cols) {
                return region.vertical_anchor == ui::VerticalAnchor::Grid;
            }
        }
        return true;
    }

    std::optional<SkiaLogicalRect> presented_cursor_rect(const SkiaFrameLayout& layout,
                                                         const ui::Scene& scene,
                                                         const SkiaAnimationFrame& frame) const {
        std::optional<SkiaLogicalPoint> position = frame.cursor_position;
        if (!position) {
            position = scene_cursor_position(layout);
            if (position && !frame.scroll_layers.empty() && scene_cursor_uses_grid_offset(scene)) {
                position->y += frame.cursor_grid_offset_y;
            }
        }
        if (!position) {
            return std::nullopt;
        }
        return SkiaLogicalRect{.x = position->x,
                               .y = position->y,
                               .width = 2.0F,
                               .height = static_cast<float>(presenter_.cell_height())};
    }

    void update_animation_targets(const ui::Scene& scene, const SkiaFrameLayout& layout,
                                  float scroll_top, const AnimationContext& context,
                                  bool geometry_changed, AnimationClock::time_point now) {
        const std::optional<SkiaLogicalPoint> target_cursor = scene_cursor_position(layout);
        if (geometry_changed || !last_scene_ || !last_scroll_top_ || !last_animation_context_ ||
            *last_animation_context_ != context) {
            scroll_animation_.reset();
            cursor_animation_.reset();
        } else if (std::abs(*last_scroll_top_ - scroll_top) > 0.0001F) {
            const float line_delta = scroll_top - *last_scroll_top_;
            if (std::abs(line_delta) <= 4) {
                const SpringState motion = scroll_animation_
                                               ? scroll_state(*scroll_animation_, now)
                                               : SpringState{.position = *last_scroll_top_};
                ScrollSceneTimeline timeline;
                if (scroll_animation_) {
                    timeline = std::move(scroll_animation_->timeline);
                }
                timeline.insert(*last_scene_, *last_scroll_top_);
                timeline.insert(scene, scroll_top);
                timeline.retain_motion_range(motion.position, scroll_top);
                scroll_animation_ = ScrollAnimation{
                    .timeline = std::move(timeline),
                    .motion = motion,
                    .target_scroll_top = scroll_top,
                    .initial_distance = std::abs(scroll_top - motion.position),
                    .sampled_at = now,
                };
            } else {
                scroll_animation_.reset();
            }
            cursor_animation_.reset();
        } else if (scroll_animation_) {
            const SpringState motion = scroll_state(*scroll_animation_, now);
            scroll_animation_->timeline.insert(scene, scroll_top);
            scroll_animation_->timeline.retain_motion_range(motion.position,
                                                            scroll_animation_->target_scroll_top);
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
        last_animation_context_ = context;
        last_cursor_target_ = target_cursor;
    }

    AnimationPresentation animation_presentation(const ui::Scene& scene,
                                                 AnimationClock::time_point now) const {
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
            const float cell_height = static_cast<float>(presenter_.cell_height());
            const std::vector<ScrollSceneLayer> layers =
                scroll_animation_->timeline.layers_at(visual_top);
            for (const ScrollSceneLayer& layer : layers) {
                presentation.frame.scroll_layers.push_back(
                    {.scene = layer.scene,
                     .grid_offset_y = (layer.scroll_top - visual_top) * cell_height,
                     .clip_top = 0.0F,
                     .clip_bottom = 0.0F});
            }
            if (vertical_layout_) {
                const float grid_bottom = vertical_layout_->grid_clip_bottom();
                const auto layer_origin = [&](std::size_t index) {
                    const SkiaScrollLayer& positioned = presentation.frame.scroll_layers[index];
                    return positioned.scene->grid_offset_rows * cell_height +
                           positioned.grid_offset_y;
                };
                for (std::size_t index = 0; index < presentation.frame.scroll_layers.size();
                     ++index) {
                    SkiaScrollLayer& positioned = presentation.frame.scroll_layers[index];
                    positioned.clip_top =
                        index == 0 ? 0.0F : std::clamp(layer_origin(index), 0.0F, grid_bottom);
                    positioned.clip_bottom =
                        index + 1 == presentation.frame.scroll_layers.size()
                            ? grid_bottom
                            : std::clamp(layer_origin(index + 1), 0.0F, grid_bottom);
                }
            }
            presentation.frame.cursor_grid_offset_y =
                (scroll_animation_->target_scroll_top - visual_top) * cell_height;
            if (scene.active_text_row && vertical_layout_) {
                presentation.frame.active_line_y =
                    vertical_layout_->row_top(*scene.active_text_row) +
                    presentation.frame.cursor_grid_offset_y;
            }
            if (!presentation.scroll_finished) {
                presentation.snapshot.active = true;
                presentation.snapshot.scroll = true;
                presentation.snapshot.scroll_progress = std::clamp(
                    1.0F - std::abs(state.position - scroll_animation_->target_scroll_top) /
                               std::max(scroll_animation_->initial_distance,
                                        scroll_position_tolerance),
                    0.0F, 1.0F);
                presentation.snapshot.scroll_velocity = state.velocity;
                presentation.snapshot.visual_scroll_top = visual_top;
                presentation.snapshot.target_scroll_top = scroll_animation_->target_scroll_top;
                presentation.snapshot.active_line_y = presentation.frame.active_line_y;
                presentation.snapshot.layers.reserve(presentation.frame.scroll_layers.size());
                for (std::size_t index = 0; index < layers.size(); ++index) {
                    presentation.snapshot.layers.push_back(
                        {.scroll_top = layers[index].scroll_top,
                         .grid_offset_y = presentation.frame.scroll_layers[index].grid_offset_y,
                         .clip_top = presentation.frame.scroll_layers[index].clip_top,
                         .clip_bottom = presentation.frame.scroll_layers[index].clip_bottom});
                }
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
        const float footer_height = presenter_.status_bar_height() + presenter_.echo_area_height();
        const float text_height = std::max(0.0F, logical_output_height_ - footer_height);
        const float visible_text_rows =
            std::clamp(text_height / cell_height, 1.0F, static_cast<float>(rows_ - 2));
        page_rows_ = std::clamp(static_cast<int>(std::floor(text_height / cell_height + 0.0001F)),
                                1, rows_ - 2);
        editor_.set_frame_rows(rows_);
        ui::Scene scene = editor_.compose(rows_, columns_, visible_text_rows);
        vertical_layout_.emplace(scene, presenter_.vertical_metrics(logical_output_height_));
        const SkiaFrameLayout frame_layout =
            presenter_.prepare_layout(scene, logical_output_width_, logical_output_height_);

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
        update_animation_targets(scene, frame_layout, scroll_top,
                                 animation_context(editor_snapshot), geometry_changed, now);
        const AnimationPresentation animation = animation_presentation(scene, now);
        const std::optional<SkiaLogicalRect> current_cursor =
            presented_cursor_rect(frame_layout, scene, animation.frame);
        const ui::SceneDamage scene_damage = damage_tracker_.update(scene, geometry_changed);
        std::vector<SkiaLogicalRect> logical_damage;
        if (animation.active) {
            logical_damage.push_back({.x = 0.0F,
                                      .y = 0.0F,
                                      .width = logical_output_width_,
                                      .height = logical_output_height_});
        } else {
            logical_damage = presenter_.damage_rects(frame_layout, scene_damage);
            append_cursor_transition_damage(logical_damage, presented_cursor_rect_, current_cursor);
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
            presenter_.render_animated(frame_layout, animation.frame, pixel_width, pixel_height,
                                       pixels_.data(), row_bytes, scale,
                                       inspection_ ? &render_diagnostics : nullptr);
        } else if (scene_damage.full_repaint) {
            presenter_.render(frame_layout, pixel_width, pixel_height, pixels_.data(), row_bytes,
                              scale, inspection_ ? &render_diagnostics : nullptr);
        } else if (!logical_damage.empty()) {
            presenter_.render_damage(frame_layout, pixel_width, pixel_height, pixels_.data(),
                                     row_bytes, logical_damage, scale);
        }

        bool full_reference_match = true;
        if (inspection_ && animation.active) {
            diagnostic_pixels_.resize(pixel_count);
            presenter_.render_animated(frame_layout, animation.frame, pixel_width, pixel_height,
                                       diagnostic_pixels_.data(), row_bytes, scale);
            full_reference_match = diagnostic_pixels_ == pixels_;
        } else if (inspection_ && !scene_damage.full_repaint) {
            diagnostic_pixels_.resize(pixel_count);
            presenter_.render(frame_layout, pixel_width, pixel_height, diagnostic_pixels_.data(),
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
        update_text_input_area(frame_layout, {.width = pixel_width, .height = pixel_height}, scale);
        SDL_RenderPresent(renderer_.get());
        presented_cursor_rect_ = current_cursor;
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
            .theme = {.canvas = theme.canvas,
                      .surface = theme.surface,
                      .raised = theme.raised,
                      .hairline = theme.hairline,
                      .active_line = theme.active_line,
                      .selection = theme.selection,
                      .text = theme.text,
                      .strong = theme.strong,
                      .muted = theme.muted,
                      .faint = theme.faint,
                      .accent = theme.accent,
                      .cursor = theme.cursor,
                      .shadow = theme.shadow,
                      .sign_added = theme.sign_added,
                      .sign_modified = theme.sign_modified,
                      .sign_deleted = theme.sign_deleted},
            .pixel_hash = hash_pixels(),
            .animation = animation,
            .damage = std::move(render_damage),
            .document_layout = std::move(document_layout),
            .popup_layout = std::move(popup_layout),
            .echo_layout = std::move(echo_layout),
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

    void update_text_input_area(const SkiaFrameLayout& layout, OutputSize output, float scale) {
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
        const std::optional<SkiaLogicalRect> cursor = presenter_.cursor_rect(layout);
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

    ui::CellPoint mouse_cell_point(float window_x, float window_y) const {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window_.get(), &window_width, &window_height);
        if (window_width <= 0 || window_height <= 0) {
            return {};
        }
        const float logical_x = window_x / static_cast<float>(window_width) * logical_output_width_;
        const float logical_y =
            window_y / static_cast<float>(window_height) * logical_output_height_;
        if (last_scene_) {
            const SkiaFrameLayout layout = presenter_.prepare_layout(
                *last_scene_, logical_output_width_, logical_output_height_);
            return presenter_.hit_test(layout, {.x = logical_x, .y = logical_y});
        }
        return {
            .row = vertical_layout_ ? vertical_layout_->row_at(logical_y) : 0,
            .column = std::clamp(static_cast<int>(std::floor(
                                     logical_x / static_cast<float>(presenter_.cell_width()))),
                                 0, columns_ - 1),
        };
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
    std::optional<AnimationContext> last_animation_context_;
    std::optional<SkiaLogicalPoint> last_cursor_target_;
    std::optional<SkiaLogicalRect> presented_cursor_rect_;
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
    bool vsync_enabled_ = false;
    std::uint64_t last_repaint_event_sequence_ = 0;
};

std::string read_file(const std::string& path) {
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
    return initial;
}

std::pair<CppIndentStyle, std::string> load_style(const std::string& path) {
    CppIndentStyle style;
    std::string style_origin = "llvm (fallback)";
    if (auto loaded = load_clang_format_style(std::filesystem::absolute(path).parent_path())) {
        style = loaded->style;
        style_origin = loaded->config_path.filename().string();
    }
    return {style, std::move(style_origin)};
}

// Renders one frame headless and writes the raw N32 (BGRA) pixels. No SDL, no
// Wayland: the presenter rasterizes into an owned buffer, the reliable way to
// capture the real chrome for font-smoothing comparisons. The vendored Skia is
// built without encoders, so a tiny external step turns the dump into a PNG.
int run_screenshot(const std::string& path, std::uint32_t initial_line,
                   const std::filesystem::path& output, SkiaFontSmoothing smoothing,
                   float font_size, int logical_width, int logical_height, float scale) {
    auto [style, style_origin] = load_style(path);
    EditorModel editor(path, read_file(path), style, std::move(style_origin), initial_line, {});
    SkiaPresenter presenter("MonoLisaCode", font_size, {}, smoothing);

    const float cell_height = static_cast<float>(presenter.cell_height());
    const float cell_width = static_cast<float>(presenter.cell_width());
    const float logical_w = static_cast<float>(logical_width);
    const float logical_h = static_cast<float>(logical_height);
    const int rows = std::max(3, static_cast<int>(std::ceil(logical_h / cell_height)));
    const int columns = std::max(20, static_cast<int>(std::ceil(logical_w / cell_width)));
    const float footer = presenter.status_bar_height() + presenter.echo_area_height();
    const float text_height = std::max(0.0F, logical_h - footer);
    const float visible_text_rows =
        std::clamp(text_height / cell_height, 1.0F, static_cast<float>(rows - 2));
    editor.set_frame_rows(rows);
    ui::Scene scene = editor.compose(rows, columns, visible_text_rows);

    const int pixel_width = static_cast<int>(std::lround(logical_w * scale));
    const int pixel_height = static_cast<int>(std::lround(logical_h * scale));
    const std::size_t row_bytes = static_cast<std::size_t>(pixel_width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(pixel_width) *
                                      static_cast<std::size_t>(pixel_height));
    presenter.render(scene, pixel_width, pixel_height, pixels.data(), row_bytes, scale);

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
               SkiaFontSmoothing smoothing, float font_size) {
    std::string initial = read_file(path);
    auto [style, style_origin] = load_style(path);

    SdlRuntime runtime;
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
        }};
    EditorModel editor(path, std::move(initial), style, std::move(style_origin), initial_line,
                       std::move(platform_services));
    SkiaPresenter presenter("MonoLisaCode", font_size, {}, smoothing);
    std::unique_ptr<InspectionHub> inspection;
    std::unique_ptr<InspectorServer> inspector;
    if (inspector_socket) {
        inspection = std::make_unique<InspectionHub>();
        inspector = std::make_unique<InspectorServer>(*inspection, *inspector_socket);
        presenter.set_show_debug_status(true);
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
    std::optional<std::filesystem::path> screenshot;
    cind::gui::SkiaFontSmoothing smoothing = cind::gui::SkiaFontSmoothing::Smooth;
    float font_size = 16.0F;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--inspect") {
                inspect = true;
            } else if (argument == "--help" || argument == "-h") {
                std::fprintf(stderr, "usage: cind-gui [--inspect] [--inspect-socket PATH] "
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
            } else if (argument == "--screenshot") {
                if (++index >= argc) {
                    throw std::runtime_error("--screenshot requires a path");
                }
                screenshot = std::filesystem::absolute(argv[index]);
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
                                 "[--font-smoothing smooth|crisp|sharp|lcd] [+LINE] <file>\n");
            return 2;
        }
        if (screenshot) {
            return cind::gui::run_screenshot(*file, initial_line, *screenshot, smoothing, font_size,
                                             900, 600, 1.5F);
        }
        if (inspect && !inspector_socket) {
            inspector_socket =
                cind::gui::default_inspector_socket_path(static_cast<int>(::getpid()));
        }
        return cind::gui::run_editor(*file, initial_line, inspector_socket, smoothing, font_size);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cind-gui: %s\n", error.what());
        return 1;
    }
}
