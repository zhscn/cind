#pragma once

#include "gui/motion.hpp"
#include "gui/scroll_timeline.hpp"
#include "gui/skia_presenter.hpp"
#include "ui/scene_damage.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cind::gui {

using FrameClock = std::chrono::steady_clock;

struct FrameIdentity {
    std::uint32_t window_slot = 0;
    std::uint32_t window_generation = 0;
    std::uint32_t view_slot = 0;
    std::uint32_t view_generation = 0;
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    std::uint64_t revision = 0;

    bool operator==(const FrameIdentity&) const = default;
};

struct FrameOutputRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct FrameOutputGeometry {
    int width = 0;
    int height = 0;
    float scale = 1.0F;
};

struct FrameLifecycle {
    bool animated = false;
    bool scroll_finished = false;
    bool view_finished = false;
};

struct FrameDamageRect {
    SkiaLogicalRect logical;
    FrameOutputRect output;
};

struct FrameScrollLayerState {
    float scroll_top = 0.0F;
    float grid_offset_y = 0.0F;
    float clip_top = 0.0F;
    float clip_bottom = 0.0F;
};

struct FrameAnimationState {
    bool active = false;
    bool scroll = false;
    bool cursor = false;
    SkiaCursorOwner cursor_owner = SkiaCursorOwner::None;
    float scroll_progress = 1.0F;
    float cursor_progress = 1.0F;
    float scroll_velocity = 0.0F;
    float visual_scroll_top = 0.0F;
    float target_scroll_top = 0.0F;
    std::vector<FrameScrollLayerState> layers;
    std::optional<float> active_line_y;
    std::optional<SkiaLogicalRect> cursor_rect;
};

struct FrameRequest {
    ui::Scene scene;
    FrameIdentity identity;
    float scroll_top = 0.0F;
    float logical_width = 0.0F;
    float logical_height = 0.0F;
    int output_width = 0;
    int output_height = 0;
    float display_scale = 1.0F;
    bool geometry_changed = false;
    FrameClock::time_point now = FrameClock::now();
};

// Immutable description of the frame that is actually presented. It owns the
// target Scene and every scroll-layer Scene referenced by its animation, so
// layout, painting, hit-testing, IME placement, damage, and inspection share
// one stable lifetime and one visual state.
class PresentedFrame {
public:
    PresentedFrame(PresentedFrame&&) noexcept = default;
    PresentedFrame& operator=(PresentedFrame&&) noexcept = default;

    PresentedFrame(const PresentedFrame&) = delete;
    PresentedFrame& operator=(const PresentedFrame&) = delete;

    const ui::Scene& scene() const { return *scene_; }
    const SkiaFrameLayout& layout() const { return *layout_; }
    const SkiaAnimationFrame& animation() const { return animation_; }
    const SkiaViewPresentation& view() const { return animation_.view; }
    const ui::SceneDamage& scene_damage() const { return scene_damage_; }
    const std::vector<SkiaLogicalRect>& logical_damage() const { return logical_damage_; }
    const std::vector<FrameDamageRect>& damage() const { return damage_; }
    const FrameAnimationState& animation_state() const { return animation_state_; }
    bool animated() const { return animated_; }
    bool full_presentation_repaint() const { return animated_ || scene_damage_.full_repaint; }
    int output_width() const { return output_width_; }
    int output_height() const { return output_height_; }
    float display_scale() const { return display_scale_; }

    std::optional<ui::HitTarget> hit_test(const SkiaPresenter& presenter,
                                          SkiaLogicalPoint point) const;

private:
    PresentedFrame(std::shared_ptr<const ui::Scene> scene,
                   std::shared_ptr<const SkiaFrameLayout> layout, SkiaAnimationFrame animation,
                   ui::SceneDamage scene_damage, std::vector<SkiaLogicalRect> logical_damage,
                   std::vector<FrameDamageRect> damage, FrameAnimationState animation_state,
                   FrameLifecycle lifecycle, FrameOutputGeometry output);

    std::shared_ptr<const ui::Scene> scene_;
    std::shared_ptr<const SkiaFrameLayout> layout_;
    SkiaAnimationFrame animation_;
    ui::SceneDamage scene_damage_;
    std::vector<SkiaLogicalRect> logical_damage_;
    std::vector<FrameDamageRect> damage_;
    FrameAnimationState animation_state_;
    bool animated_ = false;
    bool scroll_finished_ = false;
    bool view_finished_ = false;
    int output_width_ = 0;
    int output_height_ = 0;
    float display_scale_ = 1.0F;

    friend class GuiFrameController;
};

class GuiFrameController {
public:
    explicit GuiFrameController(SkiaPresenter& presenter) : presenter_(presenter) {}

    bool animations_active() const;
    PresentedFrame build(FrameRequest request);
    void did_present(const PresentedFrame& frame);
    void reset();

private:
    struct ScrollAnimation {
        ScrollSceneTimeline timeline;
        SpringState motion;
        float target_scroll_top = 0.0F;
        float initial_distance = 0.0F;
        FrameClock::time_point sampled_at;
    };
    struct ViewAnimation {
        SkiaViewPresentation from;
        SkiaViewPresentation target;
        FrameClock::time_point started;
    };
    struct AnimationPresentation;

    void update_animation_targets(const std::shared_ptr<const ui::Scene>& scene,
                                  const SkiaViewPresentation& target_view, float scroll_top,
                                  const FrameIdentity& identity, bool geometry_changed,
                                  FrameClock::time_point now);
    AnimationPresentation animation_presentation(const SkiaViewPresentation& target_view,
                                                 float logical_height,
                                                 FrameClock::time_point now) const;
    SpringState scroll_state(const ScrollAnimation& animation, FrameClock::time_point now) const;
    SkiaViewPresentation animated_view(const ViewAnimation& animation,
                                       FrameClock::time_point now) const;

    SkiaPresenter& presenter_;
    ui::SceneDamageTracker damage_tracker_;
    std::optional<ScrollAnimation> scroll_animation_;
    std::optional<ViewAnimation> view_animation_;
    std::shared_ptr<const ui::Scene> last_scene_;
    std::optional<float> last_scroll_top_;
    std::optional<FrameIdentity> last_identity_;
    std::optional<SkiaViewPresentation> last_view_target_;
    std::optional<SkiaLogicalRect> presented_cursor_rect_;
};

} // namespace cind::gui
