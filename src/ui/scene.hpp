#pragma once

#include "ui/style.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cind::ui {

// Backend-independent frame model. The editor composes a Scene per frame;
// a presenter renders it — ANSI today, a pixel canvas (Skia) or a wire
// protocol (remote frame diffs) later. Everything is already laid out in
// display columns: presenters draw, they never measure or scroll.

// A horizontal stretch of styled text on one line. Tabs are expanded and the
// text is clipped to the viewport before it gets here; `col` is the column
// inside the text area (0-based, after the gutter and sign column).
struct Run {
    int col = 0;
    std::string text;
    StyleClass style = StyleClass::Text;
    bool selected = false;
};

// Unsaved-change sign for one line (the classic gutter marks; git HEAD can
// replace "saved file" as the baseline later without touching the model).
enum class SignKind : std::uint8_t {
    None,
    Added,
    Modified,
    DeletedAbove, // lines were deleted just above this line
};

struct LineView {
    std::uint32_t line_no = 0; // 0-based document line
    SignKind sign = SignKind::None;
    std::vector<Run> runs;
};

// One row of the minimap: coarse styled spans in minimap columns — reserved
// interface, no compositor fills it yet. A TUI presenter would draw spans as
// colored dashes; a GUI presenter renders the same spans as pixel bars (the
// model is deliberately resolution-agnostic).
struct MinimapRow {
    struct Span {
        int col = 0;
        int width = 0;
        StyleClass style = StyleClass::Text;
    };
    std::uint32_t line_no = 0;
    bool in_view = false; // row falls inside the text viewport
    SignKind sign = SignKind::None;
    std::vector<Span> spans;
};

struct Scene {
    // Full terminal geometry (rows includes the status + message lines).
    int rows = 0;
    int cols = 0;

    int gutter_digits = 1; // width of the line-number column
    bool show_signs = true;

    // Visible document lines, top to bottom; fewer entries than text rows
    // means the remainder renders as past-EOF markers.
    std::vector<LineView> lines;

    // Minimap strip at the right edge; empty vector = minimap off.
    int minimap_cols = 0;
    std::vector<MinimapRow> minimap;

    std::string status_left; // path, dirty marker, position, style origin
    std::string status_key;  // keystroke caption (right-aligned)

    bool prompt_active = false;
    std::string prompt_label;
    std::string prompt_input;
    std::string message; // shown when no prompt is active

    // Terminal cursor, 1-based; compose computes it (text caret or prompt).
    int cursor_row = 1;
    int cursor_col = 1;
};

} // namespace cind::ui
