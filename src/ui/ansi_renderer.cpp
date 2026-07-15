#include "ui/ansi_renderer.hpp"

#include "ui/char_width.hpp"

#include <algorithm>
#include <format>

namespace cind::ui {

namespace {

// Theme: semantic style -> SGR attributes. The only place in the presenter
// that knows colors.
std::string_view sgr_of(StyleClass style) {
    switch (style) {
    case StyleClass::Text: return "";
    case StyleClass::Keyword: return "\x1b[1;34m";
    case StyleClass::String: return "\x1b[32m";
    case StyleClass::Number: return "\x1b[35m";
    case StyleClass::Comment: return "\x1b[90m";
    case StyleClass::Preprocessor: return "\x1b[33m";
    case StyleClass::Gutter: return "\x1b[90m";
    case StyleClass::SignAdded: return "\x1b[32m";
    case StyleClass::SignModified: return "\x1b[33m";
    case StyleClass::SignDeleted: return "\x1b[31m";
    case StyleClass::StatusBar: return "\x1b[7m";
    case StyleClass::StatusKey: return "\x1b[7m\x1b[1m";
    case StyleClass::Message: return "";
    case StyleClass::MinimapView: return "\x1b[7m";
    }
    return "";
}

struct SignGlyph {
    std::string_view text;
    StyleClass style;
};

SignGlyph sign_glyph(SignKind sign) {
    switch (sign) {
    case SignKind::Added: return {"▎", StyleClass::SignAdded};        // ▎
    case SignKind::Modified: return {"▎", StyleClass::SignModified};  // ▎
    case SignKind::DeletedAbove: return {"▔", StyleClass::SignDeleted}; // ▔
    case SignKind::None: break;
    }
    return {" ", StyleClass::Text};
}

void emit_styled(std::string& out, std::string_view text, StyleClass style, bool selected) {
    const std::string_view sgr = sgr_of(style);
    out += sgr;
    if (selected) {
        out += "\x1b[7m";
    }
    out += text;
    if (!sgr.empty() || selected) {
        out += "\x1b[0m";
    }
}

} // namespace

std::string render_ansi(const Scene& scene) {
    std::string out;
    out += "\x1b[?25l\x1b[H"; // hide cursor, home

    const int text_rows = std::max(1, scene.rows - 2);
    for (int row = 0; row < text_rows; ++row) {
        out += "\x1b[K";
        if (row < static_cast<int>(scene.lines.size())) {
            const LineView& line = scene.lines[static_cast<std::size_t>(row)];
            emit_styled(out,
                        std::format("{:>{}} ", line.line_no + 1, scene.gutter_digits),
                        StyleClass::Gutter, false);
            if (scene.show_signs) {
                const SignGlyph g = sign_glyph(line.sign);
                emit_styled(out, g.text, g.style, false);
            }
            const int text_col0 =
                scene.gutter_digits + 1 + (scene.show_signs ? 1 : 0) + 1; // 1-based
            for (const Run& run : line.runs) {
                // Runs are contiguous unless clipping split them; position
                // absolutely so gaps never smear styles.
                out += std::format("\x1b[{}G", text_col0 + run.col);
                emit_styled(out, run.text, run.style, run.selected);
            }
        } else {
            emit_styled(out, "~", StyleClass::Gutter, false);
        }
        out += "\r\n";
    }

    // Status bar: left text, fill, right-aligned keystroke caption.
    std::string status = scene.status_left;
    int fill = scene.cols - display_width(status) - display_width(scene.status_key);
    if (fill < 0) {
        status.resize(std::max<std::size_t>(
            0, status.size() + static_cast<std::size_t>(fill)));
        fill = 0;
    }
    out += sgr_of(StyleClass::StatusBar);
    out += status;
    out += std::string(static_cast<std::size_t>(fill), ' ');
    out += "\x1b[1m";
    out += scene.status_key;
    out += "\x1b[0m\r\n\x1b[K";

    // Message / prompt line.
    if (scene.prompt_active) {
        out += scene.prompt_label;
        out += scene.prompt_input;
    } else {
        out += scene.message;
    }

    out += std::format("\x1b[{};{}H\x1b[?25h", scene.cursor_row, scene.cursor_col);
    return out;
}

} // namespace cind::ui
