#pragma once

#include "ui/scene.hpp"

#include <optional>
#include <vector>

namespace cind::ui {

struct SceneRegionHeight {
    RegionRole role = RegionRole::StatusBar;
    float height = 0.0F;
};

struct SceneVerticalMetrics {
    float cell_height = 1.0F;
    float viewport_height = 0.0F;
    // Pixel-height overrides for bottom-anchored regions. Footer rows outside
    // any listed region keep cell_height; an empty list reproduces the plain
    // cell grid, which is what terminal presenters rely on.
    std::vector<SceneRegionHeight> footer_heights;
};

// Pixel heights the graphical editor chrome assigns to its bottom-anchored
// regions (modeline: cell + 12, echo strip: cell + 8). Presenters and the
// inspector share this policy so painting, hit-testing, and geometry checks
// agree; terminal presenters pass no overrides instead.
std::vector<SceneRegionHeight> editor_footer_heights(float cell_height);

// Maps scene rows into a viewport whose height may contain a fractional cell.
// Grid-anchored regions use the Scene's fractional row offset and end at the
// footer; bottom-anchored regions stack at the bottom edge with their full
// (possibly overridden) pixel heights.
class SceneVerticalLayout {
public:
    SceneVerticalLayout(const Scene& scene, const SceneVerticalMetrics& metrics);

    float row_top(int row) const;
    float row_height(int row) const;
    float grid_clip_bottom() const;
    int row_at(float y) const;
    const std::optional<int>& bottom_anchor_row() const;

private:
    int rows_ = 0;
    float cell_height_ = 1.0F;
    float viewport_height_ = 0.0F;
    float grid_offset_y_ = 0.0F;
    std::optional<int> bottom_anchor_row_;
    // Cumulative offsets of footer rows from the footer top; one entry per
    // footer row plus a trailing total, so it is empty without an anchor.
    std::vector<float> footer_offsets_;
    float footer_top_ = 0.0F;
};

} // namespace cind::ui
