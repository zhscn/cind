#include "ui/scene_layout.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cind::ui {

std::vector<SceneRegionHeight> editor_footer_heights(float cell_height) {
    return {{.role = RegionRole::StatusBar, .height = cell_height + 12.0F},
            {.role = RegionRole::EchoArea, .height = cell_height + 8.0F}};
}

SceneVerticalLayout::SceneVerticalLayout(const Scene& scene, const SceneVerticalMetrics& metrics)
    : rows_(std::max(0, scene.rows)), cell_height_(metrics.cell_height),
      viewport_height_(std::max(0.0F, metrics.viewport_height)),
      grid_offset_y_(scene.grid_offset_rows * metrics.cell_height) {
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
    if (!bottom_anchor_row_) {
        footer_top_ = viewport_height_;
        return;
    }

    const int footer_rows = rows_ - *bottom_anchor_row_;
    std::vector<float> heights(static_cast<std::size_t>(std::max(0, footer_rows)), cell_height_);
    for (const Region& region : scene.regions) {
        if (region.vertical_anchor != VerticalAnchor::Bottom || region.rect.rows <= 0) {
            continue;
        }
        for (const SceneRegionHeight& override_height : metrics.footer_heights) {
            if (override_height.role != region.role || !(override_height.height > 0.0F)) {
                continue;
            }
            const float row_height = override_height.height / static_cast<float>(region.rect.rows);
            for (int row = region.rect.row; row < region.rect.row + region.rect.rows; ++row) {
                const int index = row - *bottom_anchor_row_;
                if (index >= 0 && index < footer_rows) {
                    heights[static_cast<std::size_t>(index)] = row_height;
                }
            }
        }
    }
    footer_offsets_.reserve(heights.size() + 1);
    footer_offsets_.push_back(0.0F);
    for (const float height : heights) {
        footer_offsets_.push_back(footer_offsets_.back() + height);
    }
    footer_top_ = viewport_height_ - footer_offsets_.back();
}

float SceneVerticalLayout::row_top(int row) const {
    if (bottom_anchor_row_ && row >= *bottom_anchor_row_) {
        const int index =
            std::min(row - *bottom_anchor_row_, static_cast<int>(footer_offsets_.size()) - 1);
        return footer_top_ + footer_offsets_[static_cast<std::size_t>(index)];
    }
    return static_cast<float>(row) * cell_height_ + grid_offset_y_;
}

float SceneVerticalLayout::row_height(int row) const {
    if (bottom_anchor_row_ && row >= *bottom_anchor_row_) {
        const int index =
            std::min(row - *bottom_anchor_row_, static_cast<int>(footer_offsets_.size()) - 2);
        if (index >= 0) {
            return footer_offsets_[static_cast<std::size_t>(index) + 1] -
                   footer_offsets_[static_cast<std::size_t>(index)];
        }
    }
    return cell_height_;
}

float SceneVerticalLayout::grid_clip_bottom() const {
    return bottom_anchor_row_ ? footer_top_ : viewport_height_;
}

int SceneVerticalLayout::row_at(float y) const {
    if (rows_ == 0) {
        return 0;
    }
    int row = 0;
    if (bottom_anchor_row_ && y >= grid_clip_bottom()) {
        const float offset = y - footer_top_;
        int index = 0;
        while (index + 2 < static_cast<int>(footer_offsets_.size()) &&
               offset >= footer_offsets_[static_cast<std::size_t>(index) + 1]) {
            ++index;
        }
        row = *bottom_anchor_row_ + index;
    } else {
        row = static_cast<int>(std::floor((y - grid_offset_y_) / cell_height_));
        if (bottom_anchor_row_) {
            row = std::min(row, std::max(0, *bottom_anchor_row_ - 1));
        }
    }
    return std::clamp(row, 0, rows_ - 1);
}

const std::optional<int>& SceneVerticalLayout::bottom_anchor_row() const {
    return bottom_anchor_row_;
}

} // namespace cind::ui
