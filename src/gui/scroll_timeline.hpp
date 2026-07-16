#pragma once

#include "ui/scene.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace cind::gui {

// Retains the viewport Scenes crossed by a smooth-scroll motion. Presentation
// selects the two Scenes adjacent to the current visual document position, so
// a retargeted animation presents one continuous document canvas.
struct ScrollSceneLayer {
    std::shared_ptr<const ui::Scene> scene;
    float scroll_top = 0.0F;
};

class ScrollSceneTimeline {
public:
    void insert(ui::Scene scene, float scroll_top);
    void insert(std::shared_ptr<const ui::Scene> scene, float scroll_top);
    void retain_motion_range(float visual_scroll_top, float target_scroll_top);
    std::vector<ScrollSceneLayer> layers_at(float visual_scroll_top) const;
    std::size_t size() const;

private:
    struct Keyframe {
        std::shared_ptr<const ui::Scene> scene;
        float scroll_top = 0.0F;
    };

    std::vector<Keyframe> keyframes_;
};

} // namespace cind::gui
