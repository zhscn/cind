#include "ui/scene_layout.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cind::ui {

SceneVerticalLayout::SceneVerticalLayout(const Scene& scene, SceneVerticalMetrics metrics)
    : rows_(std::max(0, scene.rows)), cell_height_(metrics.cell_height),
      viewport_height_(std::max(0.0F, metrics.viewport_height)) {
    if (!(metrics.cell_height > 0.0F)) {
        throw std::invalid_argument("scene layout requires a positive cell height");
    }
    for (const Region& region : scene.regions) {
        if (region.vertical_anchor != VerticalAnchor::Bottom) {
            continue;
        }
        bottom_anchor_row_ =
            bottom_anchor_row_ ? std::min(*bottom_anchor_row_, region.rect.row) : region.rect.row;
    }
}

float SceneVerticalLayout::row_top(int row) const {
    float top = static_cast<float>(row) * cell_height_;
    if (bottom_anchor_row_ && row >= *bottom_anchor_row_) {
        top += viewport_height_ - static_cast<float>(rows_) * cell_height_;
    }
    return top;
}

float SceneVerticalLayout::grid_clip_bottom() const {
    return bottom_anchor_row_ ? row_top(*bottom_anchor_row_) : viewport_height_;
}

int SceneVerticalLayout::row_at(float y) const {
    if (rows_ == 0) {
        return 0;
    }
    int row = 0;
    if (bottom_anchor_row_ && y >= grid_clip_bottom()) {
        row = *bottom_anchor_row_ +
              static_cast<int>(std::floor((y - grid_clip_bottom()) / cell_height_));
    } else {
        row = static_cast<int>(std::floor(y / cell_height_));
    }
    return std::clamp(row, 0, rows_ - 1);
}

const std::optional<int>& SceneVerticalLayout::bottom_anchor_row() const {
    return bottom_anchor_row_;
}

} // namespace cind::ui
