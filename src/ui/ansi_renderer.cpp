#include "ui/ansi_renderer.hpp"

#include "ui/char_width.hpp"
#include "ui/view_tree.hpp"

#include <cmath>
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
    case StyleClass::Popup:
        return "\x1b[48;5;236m";
    }
    return "";
}

std::string_view cursor_sgr(CursorShape shape) {
    switch (shape) {
    case CursorShape::Beam:
        return "\x1b[6 q";
    case CursorShape::Block:
        return "\x1b[2 q";
    case CursorShape::Underline:
        return "\x1b[4 q";
    }
    return "\x1b[6 q";
}

} // namespace

std::string render_ansi(const Scene& scene) {
    std::string out;
    out += "\x1b[?25l\x1b[H\x1b[2J"; // hide cursor, home, clear

    const auto paint_primitive = [&](const Region& region, const Prim& prim) {
        const int content_row = region.vertical_anchor == VerticalAnchor::PaneGrid
                                    ? static_cast<int>(std::floor(region.content_offset_rows))
                                    : 0;
        out += std::format("\x1b[{};{}H", region.rect.row + prim.row + content_row + 1,
                           region.rect.col + prim.col + 1);
        const std::string_view sgr = prim.style == StyleClass::StatusBar && !region.active
                                         ? std::string_view("\x1b[48;5;236m\x1b[90m")
                                         : sgr_of(prim.style);
        out += sgr;
        if (prim.selected) {
            out += "\x1b[7m";
        }
        out += prim.text;
        if (!sgr.empty() || prim.selected) {
            out += "\x1b[0m";
        }
    };

    // Primitive regions are already cell-shaped. Semantic chrome is projected
    // into cells here, keeping its formatting policy in the terminal backend.
    const ViewTree tree(scene);
    for (const ViewLayerNode& layer : tree.layers()) {
        for (const ViewNode& node : layer.children) {
            const Region& region = scene.regions[node.region_index];
            for (const Prim& prim : region.primitives()) {
                paint_primitive(region, prim);
            }
            if (const Region::StatusContent* status = region.status()) {
                std::string left = std::format(
                    " {}{}  {}:{}  rev {}  style {} ", status->path, status->dirty ? " [+]" : "",
                    status->line, status->column, status->revision, status->style_origin);
                std::string key = status->input_state.empty()
                                      ? std::string()
                                      : std::format("[{}] ", status->input_state);
                if (!status->key.empty()) {
                    key += std::format("key: {} ", status->key);
                }
                key = std::string(clip_to_display_width(key, region.rect.cols));
                const int key_width = display_width(key);
                left = std::string(clip_to_display_width(left, region.rect.cols - key_width));
                const int fill = region.rect.cols - display_width(left) - key_width;
                paint_primitive(region,
                                {0, 0, left + std::string(static_cast<std::size_t>(fill), ' '),
                                 StyleClass::StatusBar, false, PrimKind::Text, "status:main"});
                if (!key.empty()) {
                    paint_primitive(region,
                                    {0, region.rect.cols - key_width, std::move(key),
                                     StyleClass::StatusKey, false, PrimKind::Text, "status:key"});
                }
            } else if (const Region::EchoContent* echo = region.echo()) {
                paint_primitive(region, {0, 0, echo->text, StyleClass::Message, false,
                                         PrimKind::Text, "echo:main"});
            } else if (const Region::PopupContent* popup = region.popup()) {
                std::string title =
                    std::string(clip_to_display_width(popup->title, region.rect.cols));
                title.append(static_cast<std::size_t>(region.rect.cols - display_width(title)),
                             ' ');
                paint_primitive(region, {0, 0, std::move(title), StyleClass::StatusKey, false,
                                         PrimKind::Text, "popup:title"});
                for (std::size_t offset = 0; offset < popup->items.size(); ++offset) {
                    const Region::PopupItem& item = popup->items[offset];
                    std::string row = item.detail.empty()
                                          ? item.label
                                          : std::format("{:<10} {}", item.label, item.detail);
                    row = std::string(clip_to_display_width(row, region.rect.cols));
                    row.append(static_cast<std::size_t>(region.rect.cols - display_width(row)),
                               ' ');
                    paint_primitive(
                        region, {static_cast<int>(offset) + 1, 0, std::move(row), StyleClass::Popup,
                                 popup->selected_item == popup->first_item + offset, PrimKind::Text,
                                 std::format("popup:item:{}", popup->first_item + offset)});
                }
            }
        }
    }

    for (const SceneDivider& divider : scene.dividers) {
        if (divider.axis == DividerAxis::Vertical) {
            for (int offset = 0; offset < divider.length; ++offset) {
                out += std::format("\x1b[{};{}H\x1b[90m│\x1b[0m", divider.start + offset + 1,
                                   divider.position + 1);
            }
        }
    }

    if (scene.cursor_visible) {
        out += std::format("\x1b[{};{}H{}\x1b[?25h", scene.cursor_row, scene.cursor_col,
                           cursor_sgr(scene.cursor_shape));
    }
    return out;
}

} // namespace cind::ui
