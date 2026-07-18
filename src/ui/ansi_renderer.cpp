#include "ui/ansi_renderer.hpp"

#include "ui/char_width.hpp"
#include "ui/view_tree.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cind::ui {

namespace {

std::string color_sgr(std::uint32_t argb, bool background = false, bool strong = false) {
    const std::uint32_t red = (argb >> 16U) & 0xFFU;
    const std::uint32_t green = (argb >> 8U) & 0xFFU;
    const std::uint32_t blue = argb & 0xFFU;
    return std::format("\x1b[{};2;{};{};{}{}m", background ? 48 : 38, red, green, blue,
                       strong ? ";1" : "");
}

std::string foreground_background(std::uint32_t foreground, std::uint32_t background,
                                  bool strong = false) {
    return color_sgr(foreground, false, strong) + color_sgr(background, true);
}

// Theme: semantic style -> SGR attributes. The only place in the presenter
// that knows terminal color encoding.
std::string sgr_of(StyleClass style, const PresentationTheme& theme) {
    switch (style) {
    case StyleClass::Text:
        return color_sgr(theme.text);
    case StyleClass::Keyword:
        return color_sgr(theme.salient, false, true);
    case StyleClass::String:
    case StyleClass::Number:
        return color_sgr(theme.popout);
    case StyleClass::Comment:
        return color_sgr(theme.faded);
    case StyleClass::Preprocessor:
        return color_sgr(theme.critical);
    case StyleClass::Gutter:
        return color_sgr(theme.faint);
    case StyleClass::SignAdded:
        return color_sgr(theme.sign_added);
    case StyleClass::SignModified:
        return color_sgr(theme.sign_modified);
    case StyleClass::SignDeleted:
        return color_sgr(theme.sign_deleted);
    case StyleClass::StatusBar:
        return foreground_background(theme.text, theme.band);
    case StyleClass::StatusKey:
        return foreground_background(theme.strong, theme.band, true);
    case StyleClass::Message:
        return color_sgr(theme.text);
    case StyleClass::Popup:
        return foreground_background(theme.text, theme.band);
    case StyleClass::PositionHint:
        return foreground_background(theme.canvas, theme.salient, true);
    }
    return color_sgr(theme.text);
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

std::string render_ansi(const Scene& scene, const PresentationTheme& theme) {
    std::string out;
    out += "\x1b[?25l\x1b[H\x1b[2J"; // hide cursor, home, clear

    const auto paint_primitive = [&](const Region& region, const Prim& prim) {
        const int content_row = region.vertical_anchor == VerticalAnchor::PaneGrid
                                    ? static_cast<int>(std::floor(region.content_offset_rows))
                                    : 0;
        out += std::format("\x1b[{};{}H", region.rect.row + prim.row + content_row + 1,
                           region.rect.col + prim.col + 1);
        const std::string sgr = prim.style == StyleClass::StatusBar && !region.active
                                    ? foreground_background(theme.faded, theme.highlight)
                                    : sgr_of(prim.style, theme);
        out += sgr;
        if (prim.selected) {
            out += "\x1b[7m";
        }
        out += prim.text;
        if (prim.kind == PrimKind::PositionHint) {
            const int fill = std::max(0, prim.span_cols - display_width(prim.text));
            out.append(static_cast<std::size_t>(fill), ' ');
        }
        if (!sgr.empty() || prim.selected) {
            out += "\x1b[0m";
        }
    };

    // Primitive regions are already cell-shaped. Semantic chrome is projected
    // into cells here; its content and segment ordering are already resolved.
    const ViewTree tree(scene);
    for (const ViewLayerNode& layer : tree.layers()) {
        for (const ViewNode& node : layer.children) {
            const Region& region = scene.regions[node.region_index];
            for (const Prim& prim : region.primitives()) {
                paint_primitive(region, prim);
            }
            if (const ModelineContent* status = region.status()) {
                std::string left;
                std::string right;
                const auto append = [](std::string& group, std::string_view text) {
                    if (!group.empty()) {
                        group += "  ";
                    }
                    group += text;
                };
                for (const ModelineSegment& segment : status->segments) {
                    if (segment.group == ModelineGroup::Right) {
                        append(right, segment.text);
                    } else {
                        append(left, segment.text);
                    }
                }
                left = left.empty() ? std::string() : std::format(" {} ", left);
                right = right.empty() ? std::string() : std::format(" {} ", right);
                right = std::string(clip_to_display_width(right, region.rect.cols));
                const int right_width = display_width(right);
                left = std::string(clip_to_display_width(left, region.rect.cols - right_width));
                const int fill = region.rect.cols - display_width(left) - right_width;
                paint_primitive(region,
                                {0, 0, left + std::string(static_cast<std::size_t>(fill), ' '),
                                 StyleClass::StatusBar, false, PrimKind::Text, "status:main"});
                if (!right.empty()) {
                    paint_primitive(region,
                                    {0, region.rect.cols - right_width, std::move(right),
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
                out += std::format("\x1b[{};{}H{}│\x1b[0m", divider.start + offset + 1,
                                   divider.position + 1, color_sgr(theme.faint));
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
