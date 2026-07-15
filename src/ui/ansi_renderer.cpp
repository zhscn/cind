#include "ui/ansi_renderer.hpp"

#include <format>

namespace cind::ui {

namespace {

// Theme: semantic style -> SGR attributes. The only place in the presenter
// that knows colors.
std::string_view sgr_of(StyleClass style) {
    switch (style) {
    case StyleClass::Text:
        return "";
    case StyleClass::Keyword:
        return "\x1b[1;34m";
    case StyleClass::String:
        return "\x1b[32m";
    case StyleClass::Number:
        return "\x1b[35m";
    case StyleClass::Comment:
        return "\x1b[90m";
    case StyleClass::Preprocessor:
        return "\x1b[33m";
    case StyleClass::Gutter:
        return "\x1b[90m";
    case StyleClass::SignAdded:
        return "\x1b[32m";
    case StyleClass::SignModified:
        return "\x1b[33m";
    case StyleClass::SignDeleted:
        return "\x1b[31m";
    case StyleClass::StatusBar:
        return "\x1b[7m";
    case StyleClass::StatusKey:
        return "\x1b[7m\x1b[1m";
    case StyleClass::Message:
        return "";
    }
    return "";
}

} // namespace

std::string render_ansi(const Scene& scene) {
    std::string out;
    out += "\x1b[?25l\x1b[H\x1b[2J"; // hide cursor, home, clear

    // Paint every region's display list at absolute positions. The frame is
    // rebuilt whole each time and written in one flush, so ordering between
    // regions is irrelevant as long as rects do not overlap.
    for (const Region& region : scene.regions) {
        for (const Prim& prim : region.prims) {
            out += std::format("\x1b[{};{}H", region.rect.row + prim.row + 1,
                               region.rect.col + prim.col + 1);
            const std::string_view sgr = sgr_of(prim.style);
            out += sgr;
            if (prim.selected) {
                out += "\x1b[7m";
            }
            out += prim.text;
            if (!sgr.empty() || prim.selected) {
                out += "\x1b[0m";
            }
        }
    }

    if (scene.cursor_visible) {
        out += std::format("\x1b[{};{}H\x1b[?25h", scene.cursor_row, scene.cursor_col);
    }
    return out;
}

} // namespace cind::ui
