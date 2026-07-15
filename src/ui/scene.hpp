#pragma once

#include "ui/style.hpp"

#include <cstdint>
#include <optional>
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
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
    Popup,
};

enum class VerticalAnchor : std::uint8_t {
    Grid,
    Bottom,
    Overlay,
};

struct Region {
    struct PopupItem {
        std::string label;
        std::string detail;
    };
    struct PopupContent {
        std::string title;
        std::optional<std::string> input;
        std::vector<PopupItem> items;
    };
    struct StatusContent {
        std::string path;
        bool dirty = false;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        std::uint32_t line_count = 0;
        std::uint64_t revision = 0;
        std::string style_origin;
        std::string key;
    };

    Region() = default;
    Region(RegionRole role, Rect rect, std::vector<Prim> prims,
           SurfaceClass surface = SurfaceClass::Editor,
           VerticalAnchor vertical_anchor = VerticalAnchor::Grid,
           std::optional<PopupContent> popup = std::nullopt)
        : role(role), rect(rect), prims(std::move(prims)), surface(surface),
          vertical_anchor(vertical_anchor), popup(std::move(popup)) {}

    RegionRole role = RegionRole::TextArea;
    Rect rect;
    std::vector<Prim> prims;
    SurfaceClass surface = SurfaceClass::Editor;
    VerticalAnchor vertical_anchor = VerticalAnchor::Grid;

    // Structured popup content lets graphical presenters use independent
    // spacing and typography while terminal presenters keep consuming the
    // cell primitives above. Items correspond to primitives 1..N; primitive
    // zero is the popup title.
    std::optional<PopupContent> popup;

    // Structured status-bar content for graphical presenters, mirroring the
    // popup arrangement: terminal presenters keep consuming the primitives.
    std::optional<StatusContent> status;
};

struct Scene {
    // Full frame geometry in cells. Grid regions may start at a fractional
    // row above the viewport; bottom-anchored regions remain fixed.
    int rows = 0;
    int cols = 0;
    float grid_offset_rows = 0.0F;

    // Zero-based scene row occupied by the document caret. This remains
    // available while an overlay owns the visible input cursor.
    std::optional<int> active_text_row;

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
