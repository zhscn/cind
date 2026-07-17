#pragma once

#include "ui/scene.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cind::ui {

struct SceneDamage {
    bool full_repaint = false;
    bool grid_transform_changed = false;
    float grid_translation_rows = 0.0F;
    std::size_t damaged_cells = 0;
    std::vector<Rect> cell_rects;
    std::vector<CellPoint> cursor_cells;
};

class SceneDamageTracker {
public:
    SceneDamage update(const Scene& scene, bool force_full_repaint = false);
    void reset();

private:
    int rows_ = 0;
    int cols_ = 0;
    float grid_offset_rows_ = 0.0F;
    bool initialized_ = false;
    std::vector<std::string> cells_;
    std::optional<CellPoint> cursor_;
    CursorShape cursor_shape_ = CursorShape::Beam;
    std::vector<Rect> overlay_rects_;
    std::vector<Rect> footer_rects_;
};

} // namespace cind::ui
