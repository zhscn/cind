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

std::uint32_t composite_alpha(std::uint32_t foreground, std::uint32_t background,
                              std::uint8_t opacity) {
    const std::uint32_t source_alpha = (foreground >> 24U) & 0xFFU;
    const std::uint32_t alpha = source_alpha * opacity / 255U;
    const auto channel = [&](unsigned shift) {
        const std::uint32_t source = (foreground >> shift) & 0xFFU;
        const std::uint32_t target = (background >> shift) & 0xFFU;
        return (source * alpha + target * (255U - alpha) + 127U) / 255U;
    };
    return 0xFF000000U | (channel(16U) << 16U) | (channel(8U) << 8U) | channel(0U);
}

// Encodes an already-resolved presentation style as terminal SGR. Alpha is
// composited because terminals accept RGB colors rather than alpha channels.
std::string sgr_of(const PresentationTextStyle& style, const PresentationTheme& theme,
                   std::uint8_t opacity = 255) {
    const std::uint32_t background = style.background.value_or(theme.canvas);
    const std::uint32_t foreground = composite_alpha(style.foreground, background, opacity);
    const bool strong = style.weight == PresentationWeight::Strong;
    if (style.background) {
        return foreground_background(foreground, *style.background, strong);
    }
    return color_sgr(foreground, false, strong);
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

std::string render_ansi(const Scene& scene, const PresentationTheme& theme,
                        const PresentationStyleSheet& styles) {
    std::string out;
    out += "\x1b[?25l\x1b[H\x1b[2J"; // hide cursor, home, clear

    const auto paint_primitive = [&](const Region& region, const Prim& prim) {
        const int content_row = region.vertical_anchor == VerticalAnchor::PaneGrid
                                    ? static_cast<int>(std::floor(region.content_offset_rows))
                                    : 0;
        out += std::format("\x1b[{};{}H", region.rect.row + prim.row + content_row + 1,
                           region.rect.col + prim.col + 1);
        const bool inactive_modeline = !region.active && (prim.style == StyleClass::StatusBar ||
                                                          prim.style == StyleClass::StatusKey);
        const PresentationTextRole role = inactive_modeline ? PresentationTextRole::ModelineInactive
                                                            : presentation_role(prim.style);
        const PresentationTextStyle& style = styles.style(role);
        const std::string sgr =
            sgr_of(style, theme, region.active || inactive_modeline ? 255 : styles.inactive_alpha);
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
                const bool band = popup->presentation == Region::PopupPresentation::Band;
                if (band) {
                    std::string title =
                        std::string(clip_to_display_width(popup->title, region.rect.cols));
                    title.append(static_cast<std::size_t>(region.rect.cols - display_width(title)),
                                 ' ');
                    paint_primitive(region, {0, 0, std::move(title), StyleClass::StatusKey, false,
                                             PrimKind::Text, "popup:title"});
                }
                for (std::size_t offset = 0; offset < popup->items.size(); ++offset) {
                    const Region::PopupItem& item = popup->items[offset];
                    const std::string_view detail =
                        !item.detail.empty() ? std::string_view(item.detail)
                        : popup->presentation == Region::PopupPresentation::Completion
                            ? std::string_view(item.kind)
                            : std::string_view{};
                    std::string row =
                        detail.empty() ? item.label : std::format("{:<10} {}", item.label, detail);
                    row = std::string(clip_to_display_width(row, region.rect.cols));
                    row.append(static_cast<std::size_t>(region.rect.cols - display_width(row)),
                               ' ');
                    paint_primitive(
                        region,
                        {static_cast<int>(offset) + (band ? 1 : 0), 0, std::move(row),
                         StyleClass::Popup, popup->selected_item == popup->first_item + offset,
                         PrimKind::Text, std::format("popup:item:{}", popup->first_item + offset)});
                }
            }
        }
    }

    for (const SceneDivider& divider : scene.dividers) {
        if (divider.axis == DividerAxis::Vertical) {
            for (int offset = 0; offset < divider.length; ++offset) {
                out += std::format("\x1b[{};{}H{}│\x1b[0m", divider.start + offset + 1,
                                   divider.position + 1, color_sgr(theme.divider));
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
