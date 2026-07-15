#pragma once

#include "ui/style.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cind::ui {

// Backend-independent frame model, decomposed the way UI stacks are:
// *layout* partitions the screen into regions (boxes with a role), *paint*
// fills each region with a display list of styled primitives in local
// coordinates. The editor composes a Scene per frame; a presenter renders
// it — ANSI today, a pixel canvas (Skia) or a wire protocol (remote frame
// diffs) later.
//
// Extensibility contract: a new widget (minimap, scroll bar, fold strip,
// tooltip panel, ...) is a new region role painted with the same primitives
// — compose lays it out, presenters render it with zero changes. A
// presenter specializes on a role only when it wants native drawing (e.g.
// Skia painting a pixel minimap from richer editor-side data instead of the
// cell-grid primitives).

// Cell-unit rectangle in scene coordinates (0-based row/col). A GUI
// presenter maps cells to pixels with its font metrics.
struct Rect {
    int row = 0;
    int col = 0;
    int rows = 0;
    int cols = 0;
};

// One painted primitive: styled text at a region-local position. Layout is
// done — the text is clipped, tabs are expanded, wide glyphs measured.
// StyleClass is the semantic hook: pixel presenters may key on it (draw a
// change sign as a colored bar) and ignore the glyph text.
struct Prim {
    int row = 0; // region-local
    int col = 0;
    std::string text;
    StyleClass style = StyleClass::Text;
    bool selected = false;
};

enum class RegionRole : std::uint8_t {
    TextArea,
    LineNumbers,
    ChangeSigns,
    StatusBar,
    EchoArea, // message / prompt line
};

struct Region {
    RegionRole role = RegionRole::TextArea;
    Rect rect;
    std::vector<Prim> prims;
};

struct Scene {
    // Full terminal geometry in cells.
    int rows = 0;
    int cols = 0;

    std::vector<Region> regions;

    // Terminal cursor, 1-based scene coordinates; compose computes it
    // (text caret or prompt input point).
    int cursor_row = 1;
    int cursor_col = 1;

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
