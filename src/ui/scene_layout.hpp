#pragma once

#include "ui/scene.hpp"

#include <optional>

namespace cind::ui {

struct SceneVerticalMetrics {
    float cell_height = 1.0F;
    float viewport_height = 0.0F;
};

// Maps scene rows into a viewport whose height may contain a fractional cell.
// Grid-anchored regions use the Scene's fractional row offset and end at the
// footer; bottom-anchored regions retain their full cell height at the edge.
class SceneVerticalLayout {
public:
    SceneVerticalLayout(const Scene& scene, SceneVerticalMetrics metrics);

    float row_top(int row) const;
    float grid_clip_bottom() const;
    int row_at(float y) const;
    const std::optional<int>& bottom_anchor_row() const;

private:
    int rows_ = 0;
    float cell_height_ = 1.0F;
    float viewport_height_ = 0.0F;
    float grid_offset_y_ = 0.0F;
    std::optional<int> bottom_anchor_row_;
};

} // namespace cind::ui
