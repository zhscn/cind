#pragma once

#include "ui/style.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cind::ui {

// Backend-independent frame model. Layout partitions the screen into regions;
// each region carries a surface class, a vertical anchoring policy, and a
// display list in local coordinates. Region roles remain semantic inspection
// and input-routing metadata.

// Cell-unit rectangle in scene coordinates (0-based row/col). A GUI
// presenter maps cells to pixels with its font metrics.
struct Rect {
    int row = 0;
    int col = 0;
    int rows = 0;
    int cols = 0;
};

struct CellPoint {
    int row = 0;
    int column = 0;
};

enum class PrimKind : std::uint8_t {
    Text,
    ChangeBar,
    ChangeDeletion,
};

// One painted primitive at a region-local position. Text is clipped, tabs are
// expanded, and grapheme widths are measured before the scene is published.
struct Prim {
    Prim() = default;
    Prim(int row, int col, std::string text, StyleClass style, bool selected,
         PrimKind kind = PrimKind::Text, std::string id = {})
        : row(row), col(col), text(std::move(text)), style(style), selected(selected), kind(kind),
          id(std::move(id)) {}

    int row = 0; // region-local
    int col = 0;
    std::string text;
    StyleClass style = StyleClass::Text;
    bool selected = false;
    PrimKind kind = PrimKind::Text;
    std::string id;
};

enum class SurfaceClass : std::uint8_t {
    Editor,
    Gutter,
    Status,
    Echo,
};

enum class RegionRole : std::uint8_t {
    TextArea,
    LineNumbers,
    ChangeSigns,
    StatusBar,
    EchoArea, // message / prompt line
};

enum class VerticalAnchor : std::uint8_t {
    Grid,
    Bottom,
};

struct Region {
    RegionRole role = RegionRole::TextArea;
    Rect rect;
    std::vector<Prim> prims;
    SurfaceClass surface = SurfaceClass::Editor;
    VerticalAnchor vertical_anchor = VerticalAnchor::Grid;
};

struct Scene {
    // Full frame geometry in cells. Grid regions may start at a fractional
    // row above the viewport; bottom-anchored regions remain fixed.
    int rows = 0;
    int cols = 0;
    float grid_offset_rows = 0.0F;

    std::vector<Region> regions;

    // Terminal cursor, 1-based scene coordinates; compose computes it
    // (text caret or prompt input point).
    int cursor_row = 1;
    int cursor_col = 1;
    bool cursor_visible = true;

    const Region* find(RegionRole role) const {
        for (const Region& r : regions) {
            if (r.role == role) {
                return &r;
            }
        }
        return nullptr;
    }
};

} // namespace cind::ui
