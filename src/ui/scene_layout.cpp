#include "ui/scene_layout.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cind::ui {

std::vector<SceneRegionHeight> editor_footer_heights(float cell_height,
                                                     const PresentationMetrics& metrics) {
    return {{.role = RegionRole::StatusBar, .height = cell_height + metrics.modeline_extra_height},
            {.role = RegionRole::EchoArea, .height = cell_height + metrics.echo_extra_height}};
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

ScenePixelLayout::ScenePixelLayout(const Scene& scene, const SceneVerticalMetrics& metrics,
                                   float cell_width)
    : scene_(&scene), vertical_(scene, metrics), cell_width_(cell_width),
      cell_height_(metrics.cell_height) {
    if (!(cell_width > 0.0F)) {
        throw std::invalid_argument("scene pixel layout requires a positive cell width");
    }
    for (const SceneRegionHeight& height : metrics.footer_heights) {
        if (height.role == RegionRole::StatusBar && height.height > 0.0F) {
            modeline_height_ = height.height;
            break;
        }
    }
    if (!(modeline_height_ > 0.0F)) {
        modeline_height_ = cell_height_;
    }
    workspace_rows_ = vertical_.bottom_anchor_row().value_or(scene.rows);
    for (const ScenePane& pane : scene.panes) {
        workspace_rows_ = std::max(workspace_rows_, pane.rect.row + pane.rect.rows);
    }
    workspace_rows_ = std::max(1, workspace_rows_);
}

const SceneVerticalLayout& ScenePixelLayout::vertical() const {
    return vertical_;
}

float ScenePixelLayout::workspace_row_y(int row) const {
    const float ratio = static_cast<float>(std::clamp(row, 0, workspace_rows_)) /
                        static_cast<float>(workspace_rows_);
    return ratio * vertical_.grid_clip_bottom();
}

ScenePixelRect ScenePixelLayout::pane_rect(const ScenePane& pane) const {
    const float left = static_cast<float>(pane.rect.col) * cell_width_;
    const float top = workspace_row_y(pane.rect.row);
    const float right = static_cast<float>(pane.rect.col + pane.rect.cols) * cell_width_;
    const float bottom = workspace_row_y(pane.rect.row + pane.rect.rows);
    return {.x = left,
            .y = top,
            .width = std::max(0.0F, right - left),
            .height = std::max(0.0F, bottom - top)};
}

ScenePixelRect ScenePixelLayout::region_rect(const Region& region) const {
    const float left = static_cast<float>(region.rect.col) * cell_width_;
    const float right = static_cast<float>(region.rect.col + region.rect.cols) * cell_width_;
    float top = static_cast<float>(region.rect.row) * cell_height_;
    float bottom = static_cast<float>(region.rect.row + region.rect.rows) * cell_height_;
    if (region.vertical_anchor == VerticalAnchor::PaneGrid ||
        region.vertical_anchor == VerticalAnchor::Cell) {
        const auto pane = std::ranges::find(scene_->panes, region.pane_id, &ScenePane::id);
        if (pane != scene_->panes.end()) {
            const ScenePixelRect pane_bounds = pane_rect(*pane);
            const float modeline_height = std::min(pane_bounds.height, modeline_height_);
            const float modeline_top = pane_bounds.bottom() - modeline_height;
            if (region.role == RegionRole::StatusBar &&
                region.vertical_anchor == VerticalAnchor::Cell) {
                top = modeline_top;
                bottom = pane_bounds.bottom();
            } else {
                top = pane_bounds.y +
                      static_cast<float>(region.rect.row - pane->rect.row) * cell_height_;
                const bool reaches_modeline =
                    region.rect.row + region.rect.rows >= pane->rect.row + pane->rect.rows - 1;
                bottom = reaches_modeline
                             ? modeline_top
                             : std::min(modeline_top,
                                        top + static_cast<float>(region.rect.rows) * cell_height_);
            }
        }
    } else if (region.vertical_anchor == VerticalAnchor::Bottom) {
        top = vertical_.row_top(region.rect.row);
        bottom = vertical_.row_top(region.rect.row + region.rect.rows);
    } else if (region.vertical_anchor == VerticalAnchor::Grid) {
        top = vertical_.row_top(region.rect.row);
        bottom = vertical_.row_top(region.rect.row + region.rect.rows);
        if (vertical_.bottom_anchor_row()) {
            bottom = std::min(bottom, vertical_.grid_clip_bottom());
        }
    }
    return {.x = left,
            .y = top,
            .width = std::max(0.0F, right - left),
            .height = std::max(0.0F, bottom - top)};
}

} // namespace cind::ui
