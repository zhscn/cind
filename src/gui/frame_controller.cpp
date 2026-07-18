#include "gui/frame_controller.hpp"

#include "gui/motion.hpp"
#include "ui/scene_layout.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace cind::gui {

namespace {

float animation_progress(FrameClock::time_point started, std::chrono::milliseconds duration,
                         FrameClock::time_point now) {
    const auto elapsed = std::chrono::duration<float>(now - started);
    const auto total = std::chrono::duration<float>(duration);
    return std::clamp(elapsed.count() / total.count(), 0.0F, 1.0F);
}

float ease_out_cubic(float progress) {
    const float remaining = 1.0F - progress;
    return 1.0F - remaining * remaining * remaining;
}

bool same_rect(const SkiaLogicalRect& left, const SkiaLogicalRect& right) {
    constexpr float tolerance = 0.01F;
    return std::abs(left.x - right.x) < tolerance && std::abs(left.y - right.y) < tolerance &&
           std::abs(left.width - right.width) < tolerance &&
           std::abs(left.height - right.height) < tolerance;
}

bool touches_or_intersects(const FrameOutputRect& left, const FrameOutputRect& right) {
    return left.x <= right.x + right.width && right.x <= left.x + left.width &&
           left.y <= right.y + right.height && right.y <= left.y + left.height;
}

FrameOutputRect joined(const FrameOutputRect& left, const FrameOutputRect& right) {
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

std::vector<FrameDamageRect> presentation_damage(std::span<const SkiaLogicalRect> logical_rects,
                                                 int pixel_width, int pixel_height, float scale) {
    std::vector<FrameDamageRect> result;
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

        FrameDamageRect next{
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

} // namespace

struct GuiFrameController::AnimationPresentation {
    bool active = false;
    bool scroll_finished = false;
    bool view_finished = false;
    SkiaAnimationFrame frame;
    FrameAnimationState state;
};

PresentedFrame::PresentedFrame(std::shared_ptr<const ui::Scene> scene,
                               std::shared_ptr<const SkiaFrameLayout> layout,
                               SkiaPreparedAnimationFrame animation, ui::SceneDamage scene_damage,
                               std::vector<SkiaLogicalRect> logical_damage,
                               std::vector<FrameDamageRect> damage,
                               FrameAnimationState animation_state, FrameLifecycle lifecycle,
                               FrameOutputGeometry output)
    : scene_(std::move(scene)), layout_(std::move(layout)), animation_(std::move(animation)),
      scene_damage_(std::move(scene_damage)), logical_damage_(std::move(logical_damage)),
      damage_(std::move(damage)), animation_state_(std::move(animation_state)),
      animated_(lifecycle.animated), scroll_finished_(lifecycle.scroll_finished),
      view_finished_(lifecycle.view_finished), output_width_(output.width),
      output_height_(output.height), display_scale_(output.scale) {}

std::optional<ui::HitTarget> PresentedFrame::hit_test(const SkiaPresenter& presenter,
                                                      SkiaLogicalPoint point) const {
    if (const std::optional<ui::ViewHit> fixed =
            presenter.hit_test_view(*layout_, point, SkiaHitTestScope::Fixed)) {
        return ui::resolve_hit_target(*scene_, *fixed);
    }
    for (const SkiaPreparedScrollLayer& layer : animation_.scroll_layers) {
        if (!layer.scene || !layer.layout || point.y < layer.clip_top ||
            point.y >= layer.clip_bottom) {
            continue;
        }
        const std::optional<ui::ViewHit> hit = presenter.hit_test_view(
            *layer.layout, {.x = point.x, .y = point.y - layer.grid_offset_y},
            SkiaHitTestScope::Grid);
        return hit ? ui::resolve_hit_target(*layer.scene, *hit) : std::nullopt;
    }
    const std::optional<ui::ViewHit> hit =
        presenter.hit_test_view(*layout_, point, SkiaHitTestScope::Grid);
    return hit ? ui::resolve_hit_target(*scene_, *hit) : std::nullopt;
}

bool GuiFrameController::animations_active() const {
    return scroll_animation_.has_value() || view_animation_.has_value();
}

SpringState GuiFrameController::scroll_state(const ScrollAnimation& animation,
                                             FrameClock::time_point now) const {
    const float elapsed = std::chrono::duration<float>(now - animation.sampled_at).count();
    return advance_critical_spring(animation.motion,
                                   {.target = animation.target_scroll_top,
                                    .angular_frequency = motion_.scroll_spring_frequency,
                                    .elapsed_seconds = elapsed});
}

SkiaViewPresentation GuiFrameController::animated_view(const ViewAnimation& animation,
                                                       FrameClock::time_point now) const {
    const float progress = ease_out_cubic(animation_progress(
        animation.started, std::chrono::milliseconds{motion_.view_duration_ms}, now));
    return interpolate_view_presentation(animation.from, animation.target, progress);
}

std::shared_ptr<const SkiaFrameLayout>
GuiFrameController::prepared_layout(const std::shared_ptr<const ui::Scene>& scene,
                                    float viewport_width, float viewport_height) {
    const auto cached = std::ranges::find_if(layout_cache_, [&](const CachedLayout& entry) {
        return entry.scene == scene && entry.viewport_width == viewport_width &&
               entry.viewport_height == viewport_height;
    });
    if (cached != layout_cache_.end()) {
        return cached->layout;
    }

    auto layout = std::make_shared<const SkiaFrameLayout>(
        presenter_.prepare_layout(*scene, viewport_width, viewport_height));
    layout_cache_.push_back({.scene = scene,
                             .layout = layout,
                             .viewport_width = viewport_width,
                             .viewport_height = viewport_height});
    return layout;
}

SkiaPreparedAnimationFrame
GuiFrameController::prepare_animation(const SkiaAnimationFrame& animation, float viewport_width,
                                      float viewport_height) {
    SkiaPreparedAnimationFrame result;
    result.view = animation.view;
    result.scroll_layers.reserve(animation.scroll_layers.size());
    for (const SkiaScrollLayer& layer : animation.scroll_layers) {
        if (!layer.scene) {
            throw std::invalid_argument("GUI animation scroll layer has no Scene");
        }
        result.scroll_layers.push_back(
            {.scene = layer.scene,
             .layout = prepared_layout(layer.scene, viewport_width, viewport_height),
             .grid_offset_y = layer.grid_offset_y,
             .clip_top = layer.clip_top,
             .clip_bottom = layer.clip_bottom});
    }
    return result;
}

void GuiFrameController::retain_presented_layouts(const std::shared_ptr<const ui::Scene>& scene,
                                                  const SkiaPreparedAnimationFrame& animation) {
    std::erase_if(layout_cache_, [&](const CachedLayout& entry) {
        if (entry.scene == scene) {
            return false;
        }
        return std::ranges::none_of(
            animation.scroll_layers, [&](const SkiaPreparedScrollLayer& layer) {
                return layer.scene == entry.scene && layer.layout == entry.layout;
            });
    });
}

void GuiFrameController::update_animation_targets(const std::shared_ptr<const ui::Scene>& scene,
                                                  const SkiaViewPresentation& target_view,
                                                  float scroll_top, const FrameIdentity& identity,
                                                  bool animate_scroll, bool geometry_changed,
                                                  FrameClock::time_point now) {
    // A workspace contains independently scrolling pane grids; the scalar
    // scroll timeline represents a single grid only. Composite frames use
    // direct presentation and retain normal scene damage tracking.
    if (!scene->panes.empty()) {
        scroll_animation_.reset();
        view_animation_.reset();
        last_scene_ = scene;
        last_scroll_top_ = scroll_top;
        last_identity_ = identity;
        last_view_target_ = target_view;
        return;
    }
    if (geometry_changed || !last_scene_ || !last_scroll_top_ || !last_identity_ ||
        *last_identity_ != identity) {
        scroll_animation_.reset();
        view_animation_.reset();
    } else if (!animate_scroll) {
        scroll_animation_.reset();
        if (std::abs(*last_scroll_top_ - scroll_top) > 0.0001F) {
            view_animation_.reset();
        }
    } else if (std::abs(*last_scroll_top_ - scroll_top) > 0.0001F) {
        const float line_delta = scroll_top - *last_scroll_top_;
        if (std::abs(line_delta) <= 4.0F) {
            const SpringState motion = scroll_animation_
                                           ? scroll_state(*scroll_animation_, now)
                                           : SpringState{.position = *last_scroll_top_};
            ScrollSceneTimeline timeline;
            if (scroll_animation_) {
                timeline = std::move(scroll_animation_->timeline);
            }
            timeline.insert(last_scene_, *last_scroll_top_);
            timeline.insert(scene, scroll_top);
            timeline.retain_motion_range(motion.position, scroll_top);
            scroll_animation_ =
                ScrollAnimation{.timeline = std::move(timeline),
                                .motion = motion,
                                .target_scroll_top = scroll_top,
                                .initial_distance = std::abs(scroll_top - motion.position),
                                .sampled_at = now};
        } else {
            scroll_animation_.reset();
        }
        view_animation_.reset();
    } else if (scroll_animation_) {
        const SpringState motion = scroll_state(*scroll_animation_, now);
        scroll_animation_->timeline.insert(scene, scroll_top);
        scroll_animation_->timeline.retain_motion_range(motion.position,
                                                        scroll_animation_->target_scroll_top);
    } else if (target_view.cursor_rect && last_view_target_ && last_view_target_->cursor_rect &&
               (target_view.cursor_owner != last_view_target_->cursor_owner ||
                !same_rect(*last_view_target_->cursor_rect, *target_view.cursor_rect))) {
        const SkiaViewPresentation from =
            view_animation_ ? animated_view(*view_animation_, now) : *last_view_target_;
        const float maximum_x_distance = static_cast<float>(presenter_.cell_width() * 4);
        const float maximum_y_distance = static_cast<float>(presenter_.cell_height() * 2);
        if (from.cursor_rect && from.cursor_owner == target_view.cursor_owner &&
            std::abs(target_view.cursor_rect->x - from.cursor_rect->x) <= maximum_x_distance &&
            std::abs(target_view.cursor_rect->y - from.cursor_rect->y) <= maximum_y_distance) {
            view_animation_ = ViewAnimation{.from = from, .target = target_view, .started = now};
        } else {
            view_animation_.reset();
        }
    } else if (!target_view.cursor_rect) {
        view_animation_.reset();
    }

    last_scene_ = scene;
    last_scroll_top_ = scroll_top;
    last_identity_ = identity;
    last_view_target_ = target_view;
}

GuiFrameController::AnimationPresentation
GuiFrameController::animation_presentation(const SkiaViewPresentation& target_view,
                                           float logical_height, bool constrain_to_cursor,
                                           FrameClock::time_point now) const {
    AnimationPresentation presentation;
    presentation.frame.view = target_view;
    if (scroll_animation_) {
        const SpringState state = scroll_state(*scroll_animation_, now);
        const bool at_rest =
            spring_at_rest(state, {.target = scroll_animation_->target_scroll_top,
                                   .position_tolerance = motion_.scroll_position_tolerance,
                                   .velocity_tolerance = motion_.scroll_velocity_tolerance});
        float visual_top = at_rest ? scroll_animation_->target_scroll_top : state.position;
        presentation.active = true;
        presentation.scroll_finished = at_rest;
        const float cell_height = static_cast<float>(presenter_.cell_height());
        const ui::SceneVerticalLayout vertical_layout(*last_scene_,
                                                      presenter_.vertical_metrics(logical_height));
        const float grid_bottom = vertical_layout.grid_clip_bottom();
        float document_offset_y = (scroll_animation_->target_scroll_top - visual_top) * cell_height;
        if (constrain_to_cursor &&
            presentation.frame.view.cursor_owner == SkiaCursorOwner::Document &&
            presentation.frame.view.cursor_rect) {
            const SkiaLogicalRect& cursor = *presentation.frame.view.cursor_rect;
            float minimum_offset = std::min(0.0F, -cursor.y);
            float maximum_offset = std::max(0.0F, grid_bottom - (cursor.y + cursor.height));
            if (presentation.frame.view.active_line_y) {
                minimum_offset = std::max(minimum_offset,
                                          std::min(0.0F, -*presentation.frame.view.active_line_y));
                maximum_offset =
                    std::min(maximum_offset,
                             std::max(0.0F, grid_bottom - (*presentation.frame.view.active_line_y +
                                                           cell_height)));
            }
            document_offset_y = std::clamp(document_offset_y, minimum_offset, maximum_offset);
            visual_top = scroll_animation_->target_scroll_top - document_offset_y / cell_height;
        }
        if (presentation.frame.view.active_line_y) {
            *presentation.frame.view.active_line_y += document_offset_y;
        }
        if (presentation.frame.view.cursor_owner == SkiaCursorOwner::Document &&
            presentation.frame.view.cursor_rect) {
            presentation.frame.view.cursor_rect->y += document_offset_y;
        }
        const std::vector<ScrollSceneLayer> layers =
            scroll_animation_->timeline.layers_at(visual_top);
        for (const ScrollSceneLayer& layer : layers) {
            presentation.frame.scroll_layers.push_back(
                {.scene = layer.scene,
                 .grid_offset_y = (layer.scroll_top - visual_top) * cell_height,
                 .clip_top = 0.0F,
                 .clip_bottom = 0.0F});
        }
        const auto layer_origin = [&](std::size_t index) {
            const SkiaScrollLayer& positioned = presentation.frame.scroll_layers[index];
            return positioned.scene->grid_offset_rows * cell_height + positioned.grid_offset_y;
        };
        for (std::size_t index = 0; index < presentation.frame.scroll_layers.size(); ++index) {
            SkiaScrollLayer& positioned = presentation.frame.scroll_layers[index];
            positioned.clip_top =
                index == 0 ? 0.0F : std::clamp(layer_origin(index), 0.0F, grid_bottom);
            positioned.clip_bottom = index + 1 == presentation.frame.scroll_layers.size()
                                         ? grid_bottom
                                         : std::clamp(layer_origin(index + 1), 0.0F, grid_bottom);
        }
        if (!presentation.scroll_finished) {
            presentation.state.active = true;
            presentation.state.scroll = true;
            presentation.state.cursor_constrained =
                constrain_to_cursor &&
                presentation.frame.view.cursor_owner == SkiaCursorOwner::Document &&
                presentation.frame.view.cursor_rect.has_value();
            presentation.state.scroll_progress =
                std::clamp(1.0F - std::abs(state.position - scroll_animation_->target_scroll_top) /
                                      std::max(scroll_animation_->initial_distance,
                                               motion_.scroll_position_tolerance),
                           0.0F, 1.0F);
            presentation.state.scroll_velocity = state.velocity;
            presentation.state.visual_scroll_top = visual_top;
            presentation.state.target_scroll_top = scroll_animation_->target_scroll_top;
            presentation.state.layers.reserve(presentation.frame.scroll_layers.size());
            for (std::size_t index = 0; index < layers.size(); ++index) {
                presentation.state.layers.push_back(
                    {.scroll_top = layers[index].scroll_top,
                     .grid_offset_y = presentation.frame.scroll_layers[index].grid_offset_y,
                     .clip_top = presentation.frame.scroll_layers[index].clip_top,
                     .clip_bottom = presentation.frame.scroll_layers[index].clip_bottom});
            }
        }
    }
    if (view_animation_) {
        const float linear_progress = animation_progress(
            view_animation_->started, std::chrono::milliseconds{motion_.view_duration_ms}, now);
        presentation.frame.view = animated_view(*view_animation_, now);
        presentation.active = true;
        presentation.view_finished = linear_progress >= 1.0F;
        if (!presentation.view_finished) {
            presentation.state.active = true;
            presentation.state.cursor = true;
            presentation.state.cursor_progress = ease_out_cubic(linear_progress);
        }
    }
    if (presentation.state.active) {
        presentation.state.cursor_owner = presentation.frame.view.cursor_owner;
        presentation.state.active_line_y = presentation.frame.view.active_line_y;
        presentation.state.cursor_rect = presentation.frame.view.cursor_rect;
    }
    return presentation;
}

PresentedFrame GuiFrameController::build(FrameRequest request) {
    if (!(request.logical_width >= 0.0F) || !(request.logical_height >= 0.0F) ||
        request.output_width <= 0 || request.output_height <= 0 ||
        !(request.display_scale > 0.0F)) {
        throw std::invalid_argument("GUI frame request has invalid output geometry");
    }
    std::shared_ptr<const ui::Scene> scene;
    if (!request.geometry_changed && last_scene_ && *last_scene_ == request.scene) {
        scene = last_scene_;
    } else {
        scene = std::make_shared<const ui::Scene>(std::move(request.scene));
    }
    const std::shared_ptr<const SkiaFrameLayout> layout =
        prepared_layout(scene, request.logical_width, request.logical_height);
    const SkiaViewPresentation target_view = presenter_.view_presentation(*layout);
    update_animation_targets(scene, target_view, request.scroll_top, request.identity,
                             request.animate_scroll, request.geometry_changed, request.now);
    AnimationPresentation animation = animation_presentation(
        target_view, request.logical_height, request.constrain_scroll_to_cursor, request.now);

    ui::SceneDamage scene_damage = damage_tracker_.update(*scene, request.geometry_changed);
    std::vector<SkiaLogicalRect> logical_damage;
    if (animation.active) {
        logical_damage.push_back({.x = 0.0F,
                                  .y = 0.0F,
                                  .width = request.logical_width,
                                  .height = request.logical_height});
    } else {
        logical_damage = presenter_.damage_rects(*layout, scene_damage);
        append_cursor_transition_damage(logical_damage, presented_cursor_rect_,
                                        animation.frame.view.cursor_rect);
    }
    std::vector<FrameDamageRect> damage = presentation_damage(
        logical_damage, request.output_width, request.output_height, request.display_scale);
    logical_damage.clear();
    logical_damage.reserve(damage.size());
    for (const FrameDamageRect& rect : damage) {
        logical_damage.push_back(rect.logical);
    }

    SkiaPreparedAnimationFrame prepared_animation =
        prepare_animation(animation.frame, request.logical_width, request.logical_height);
    retain_presented_layouts(scene, prepared_animation);

    return PresentedFrame(std::move(scene), layout, std::move(prepared_animation),
                          std::move(scene_damage), std::move(logical_damage), std::move(damage),
                          std::move(animation.state),
                          {.animated = animation.active,
                           .scroll_finished = animation.scroll_finished,
                           .view_finished = animation.view_finished},
                          {.width = request.output_width,
                           .height = request.output_height,
                           .scale = request.display_scale});
}

void GuiFrameController::did_present(const PresentedFrame& frame) {
    presented_cursor_rect_ = frame.view().cursor_rect;
    if (frame.scroll_finished_) {
        scroll_animation_.reset();
    }
    if (frame.view_finished_) {
        view_animation_.reset();
    }
}

void GuiFrameController::reset() {
    damage_tracker_.reset();
    scroll_animation_.reset();
    view_animation_.reset();
    last_scene_.reset();
    last_scroll_top_.reset();
    last_identity_.reset();
    last_view_target_.reset();
    presented_cursor_rect_.reset();
    layout_cache_.clear();
}

} // namespace cind::gui
