#include "gui/skia_presenter.hpp"

#include "ui/char_width.hpp"
#include "ui/scene_layout.hpp"

#include <modules/skshaper/include/SkShaper.h>
#include <skia/core/SkBlurTypes.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColor.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMetrics.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkFontStyle.h>
#include <skia/core/SkFontTypes.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkMaskFilter.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkRRect.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkString.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkTextBlob.h>
#include <skia/core/SkTypeface.h>
#include <skia/ports/SkFontMgr_fontconfig.h>
#include <skia/ports/SkFontScanner_FreeType.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace cind::gui {

namespace {

// Footer chrome metrics in logical pixels over the cell grid; the footer row
// heights themselves come from ui::editor_footer_heights.
constexpr float footer_padding_x = 12.0F;
constexpr float segment_gap = 8.0F;

// Floating panel metrics.
constexpr float panel_radius = 6.0F;
constexpr float panel_padding_y = 4.0F;
constexpr float panel_padding_x = 12.0F;
constexpr float panel_header_gap = 4.0F;
constexpr float panel_shadow_reach = 20.0F;

SkColor color(std::uint32_t argb) {
    return static_cast<SkColor>(argb);
}

SkColor foreground(ui::StyleClass style, const SkiaTheme& theme) {
    switch (style) {
    case ui::StyleClass::Text:
        return color(theme.text);
    case ui::StyleClass::Keyword:
        return color(0xFF569CD6);
    case ui::StyleClass::String:
        return color(0xFF6A9955);
    case ui::StyleClass::Number:
        return color(0xFFB5CEA8);
    case ui::StyleClass::Comment:
        return color(theme.muted);
    case ui::StyleClass::Preprocessor:
        return color(0xFFDCDCAA);
    case ui::StyleClass::Gutter:
        return color(theme.faint);
    case ui::StyleClass::SignAdded:
        return color(theme.sign_added);
    case ui::StyleClass::SignModified:
        return color(theme.sign_modified);
    case ui::StyleClass::SignDeleted:
        return color(theme.sign_deleted);
    case ui::StyleClass::StatusBar:
        return color(theme.text);
    case ui::StyleClass::StatusKey:
        return color(theme.strong);
    case ui::StyleClass::Message:
        return color(theme.muted);
    case ui::StyleClass::Popup:
        return color(theme.text);
    }
    return color(theme.text);
}

SkRect pixel_rect(const ui::Rect& rect, int cell_width, int cell_height) {
    return SkRect::MakeXYWH(static_cast<SkScalar>(rect.col * cell_width),
                            static_cast<SkScalar>(rect.row * cell_height),
                            static_cast<SkScalar>(rect.cols * cell_width),
                            static_cast<SkScalar>(rect.rows * cell_height));
}

SkRect region_pixel_rect(const ui::Region& region, int cell_width, int cell_height,
                         const ui::SceneVerticalLayout& layout) {
    SkRect bounds = pixel_rect(region.rect, cell_width, cell_height);
    if (region.vertical_anchor == ui::VerticalAnchor::Overlay) {
        return bounds;
    }
    if (region.vertical_anchor == ui::VerticalAnchor::Bottom) {
        const float top = layout.row_top(region.rect.row);
        const float bottom = layout.row_top(region.rect.row + region.rect.rows);
        return SkRect::MakeLTRB(bounds.left(), top, bounds.right(), std::max(top, bottom));
    }
    const float top = layout.row_top(region.rect.row);
    float bottom = layout.row_top(region.rect.row + region.rect.rows);
    if (layout.bottom_anchor_row()) {
        bottom = std::min(bottom, layout.grid_clip_bottom());
    }
    return SkRect::MakeLTRB(bounds.left(), top, bounds.right(), std::max(top, bottom));
}

struct PopupLayout {
    const ui::Region* region = nullptr;
    const ui::Region::PopupContent* content = nullptr;
    SkRect panel;
    SkRect shadow;
    SkRect header;
    float row_height = 0.0F;
    float rows_top = 0.0F;
    std::size_t item_count = 0;
    bool input_active = false;
    std::string input;
};

struct LogicalViewport {
    float width = 0.0F;
    float height = 0.0F;
};

struct CellMetrics {
    int width = 1;
    int height = 1;
};

std::string popup_label(std::string_view title) {
    while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back())) != 0) {
        title.remove_suffix(1);
    }
    if (!title.empty() && title.back() == ':') {
        title.remove_suffix(1);
    }
    return std::string(title);
}

std::optional<PopupLayout> popup_layout(const ui::Scene& scene, LogicalViewport viewport,
                                        const ui::SceneVerticalMetrics& metrics) {
    const ui::Region* popup = scene.find(ui::RegionRole::Popup);
    if (!popup || !popup->popup) {
        return std::nullopt;
    }
    const ui::Region::PopupContent& content = popup->popup.value();
    const float cell_height = metrics.cell_height;

    const ui::SceneVerticalLayout vertical_layout(scene, metrics);
    const float grid_bottom = vertical_layout.grid_clip_bottom();
    const float margin = std::clamp(viewport.width * 0.03F, 12.0F, 24.0F);
    const float available_width = viewport.width - 2.0F * margin;
    const bool input_active = content.input.has_value();
    const float desired_width = input_active ? std::clamp(viewport.width * 0.58F, 560.0F, 840.0F)
                                             : std::clamp(viewport.width * 0.48F, 420.0F, 680.0F);
    const float panel_width = std::min(desired_width, available_width);
    const float header_height = input_active ? cell_height + 8.0F : cell_height + 2.0F;
    const float row_height = cell_height + 6.0F;
    const float picker_top = std::clamp(grid_bottom * 0.11F, margin, 96.0F);
    const float available_height = grid_bottom - picker_top - margin;
    const float fixed_height = panel_padding_y + header_height + panel_header_gap + panel_padding_y;
    if (panel_width < 240.0F || available_height < fixed_height + row_height) {
        return std::nullopt;
    }

    const std::size_t available_items = content.items.size();
    const std::size_t height_items = static_cast<std::size_t>(
        std::max(1.0F, std::floor((available_height - fixed_height) / row_height)));
    const std::size_t item_count = std::min({available_items, height_items, std::size_t{12}});
    if (item_count == 0) {
        return std::nullopt;
    }

    const float panel_height = fixed_height + static_cast<float>(item_count) * row_height;
    const float panel_x = (viewport.width - panel_width) * 0.5F;
    const float panel_y = input_active ? picker_top : grid_bottom - margin - panel_height;
    const SkRect panel = SkRect::MakeXYWH(panel_x, panel_y, panel_width, panel_height);
    const SkRect header =
        SkRect::MakeXYWH(panel.left(), panel.top() + panel_padding_y, panel.width(), header_height);
    SkRect shadow = panel;
    shadow.outset(panel_shadow_reach, panel_shadow_reach);
    shadow.offset(0.0F, 4.0F);
    shadow.intersect(SkRect::MakeWH(viewport.width, viewport.height));
    return PopupLayout{.region = popup,
                       .content = &content,
                       .panel = panel,
                       .shadow = shadow,
                       .header = header,
                       .row_height = row_height,
                       .rows_top = header.bottom() + panel_header_gap,
                       .item_count = item_count,
                       .input_active = input_active,
                       .input = content.input.value_or(std::string{})};
}

std::string popup_prompt(const ui::Region::PopupContent& popup) {
    return popup_label(popup.title) + ":";
}

std::string popup_input_text(const ui::Region::PopupContent& popup, std::string_view input) {
    std::string text = popup_prompt(popup);
    text += " ";
    text += input;
    return text;
}

float popup_text_left(const PopupLayout& layout, std::string_view text, int cell_width) {
    const float natural_width = static_cast<float>(ui::display_width(text) * cell_width);
    const float available_width = layout.header.width() - 2.0F * panel_padding_x;
    return layout.header.left() + panel_padding_x + std::min(0.0F, available_width - natural_width);
}

std::optional<SkRect> popup_cursor_rect(const PopupLayout& layout, CellMetrics cell) {
    if (!layout.input_active || layout.content == nullptr) {
        return std::nullopt;
    }
    const std::string text = popup_input_text(*layout.content, layout.input);
    const float left = popup_text_left(layout, text, cell.width);
    const float x = std::clamp(left + static_cast<float>(ui::display_width(text) * cell.width),
                               layout.header.left() + 8.0F, layout.header.right() - 3.0F);
    const float y =
        layout.header.top() + (layout.header.height() - static_cast<float>(cell.height)) * 0.5F;
    return SkRect::MakeXYWH(x, y, 2.0F, static_cast<float>(cell.height));
}

bool contains(const ui::Rect& rect, int row, int col) {
    return row >= rect.row && row < rect.row + rect.rows && col >= rect.col &&
           col < rect.col + rect.cols;
}

SkiaLogicalRect logical_rect(const SkRect& rect) {
    return {.x = rect.x(), .y = rect.y(), .width = rect.width(), .height = rect.height()};
}

bool extends_horizontally(const SkRect& bounds, const SkRect& cell) {
    constexpr SkScalar tolerance = 0.01F;
    return bounds.left() < cell.left() - tolerance || bounds.right() > cell.right() + tolerance;
}

bool extends_vertically(const SkRect& bounds, const SkRect& cell) {
    constexpr SkScalar tolerance = 0.01F;
    return bounds.top() < cell.top() - tolerance || bounds.bottom() > cell.bottom() + tolerance;
}

bool touches_or_intersects(const SkiaLogicalRect& left, const SkiaLogicalRect& right) {
    return left.x <= right.x + right.width && right.x <= left.x + left.width &&
           left.y <= right.y + right.height && right.y <= left.y + left.height;
}

SkiaLogicalRect joined(const SkiaLogicalRect& left, const SkiaLogicalRect& right) {
    const float x = std::min(left.x, right.x);
    const float y = std::min(left.y, right.y);
    const float right_edge = std::max(left.x + left.width, right.x + right.width);
    const float bottom = std::max(left.y + left.height, right.y + right.height);
    return {.x = x, .y = y, .width = right_edge - x, .height = bottom - y};
}

void append_damage(std::vector<SkiaLogicalRect>& rectangles, SkiaLogicalRect incoming) {
    for (std::size_t index = 0; index < rectangles.size();) {
        if (!touches_or_intersects(rectangles[index], incoming)) {
            ++index;
            continue;
        }
        incoming = joined(rectangles[index], incoming);
        rectangles.erase(rectangles.begin() + static_cast<std::ptrdiff_t>(index));
        index = 0;
    }
    rectangles.push_back(incoming);
}

bool same_rect(const SkiaLogicalRect& left, const SkiaLogicalRect& right) {
    constexpr float tolerance = 0.01F;
    return std::abs(left.x - right.x) < tolerance && std::abs(left.y - right.y) < tolerance &&
           std::abs(left.width - right.width) < tolerance &&
           std::abs(left.height - right.height) < tolerance;
}

struct PixelProbe {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    std::vector<std::uint32_t> before;
};

struct RasterView {
    const void* pixels = nullptr;
    std::size_t row_bytes = 0;
    int width = 0;
    int height = 0;
    float device_scale = 1.0F;
};

std::uint32_t read_pixel(const void* pixels, std::size_t row_bytes, int x, int y) {
    const auto* address = static_cast<const std::byte*>(pixels) +
                          static_cast<std::size_t>(y) * row_bytes +
                          static_cast<std::size_t>(x) * sizeof(std::uint32_t);
    std::uint32_t pixel = 0;
    std::memcpy(&pixel, address, sizeof(pixel));
    return pixel;
}

PixelProbe capture_probe(const SkRect& logical_bounds, const RasterView& raster) {
    PixelProbe probe{
        .left = std::clamp(
            static_cast<int>(std::floor(logical_bounds.left() * raster.device_scale)) - 1, 0,
            raster.width),
        .top =
            std::clamp(static_cast<int>(std::floor(logical_bounds.top() * raster.device_scale)) - 1,
                       0, raster.height),
        .right = std::clamp(
            static_cast<int>(std::ceil(logical_bounds.right() * raster.device_scale)) + 1, 0,
            raster.width),
        .bottom = std::clamp(
            static_cast<int>(std::ceil(logical_bounds.bottom() * raster.device_scale)) + 1, 0,
            raster.height),
        .before = {},
    };
    probe.before.reserve(static_cast<std::size_t>(probe.right - probe.left) *
                         static_cast<std::size_t>(probe.bottom - probe.top));
    for (int y = probe.top; y < probe.bottom; ++y) {
        for (int x = probe.left; x < probe.right; ++x) {
            probe.before.push_back(read_pixel(raster.pixels, raster.row_bytes, x, y));
        }
    }
    return probe;
}

std::optional<SkiaLogicalRect> changed_pixel_bounds(const PixelProbe& probe,
                                                    const RasterView& raster) {
    int left = probe.right;
    int top = probe.bottom;
    int right = probe.left;
    int bottom = probe.top;
    std::size_t index = 0;
    for (int y = probe.top; y < probe.bottom; ++y) {
        for (int x = probe.left; x < probe.right; ++x, ++index) {
            if (read_pixel(raster.pixels, raster.row_bytes, x, y) == probe.before[index]) {
                continue;
            }
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x + 1);
            bottom = std::max(bottom, y + 1);
        }
    }
    if (left >= right || top >= bottom) {
        return std::nullopt;
    }
    return SkiaLogicalRect{.x = static_cast<float>(left) / raster.device_scale,
                           .y = static_cast<float>(top) / raster.device_scale,
                           .width = static_cast<float>(right - left) / raster.device_scale,
                           .height = static_cast<float>(bottom - top) / raster.device_scale};
}

std::string status_basename(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string status_directory(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string status_percent(const ui::Region::StatusContent& status) {
    if (status.line_count == 0) {
        return {};
    }
    const std::uint64_t percent = std::min<std::uint64_t>(
        100, static_cast<std::uint64_t>(status.line) * 100 / status.line_count);
    return std::format("{}%", percent);
}

float footer_height_for(ui::RegionRole role, float cell_height) {
    for (const ui::SceneRegionHeight& height : ui::editor_footer_heights(cell_height)) {
        if (height.role == role) {
            return height.height;
        }
    }
    return cell_height;
}

} // namespace

void append_cursor_transition_damage(std::vector<SkiaLogicalRect>& damage,
                                     const std::optional<SkiaLogicalRect>& previous_cursor,
                                     const std::optional<SkiaLogicalRect>& current_cursor) {
    if ((!previous_cursor && !current_cursor) ||
        (previous_cursor && current_cursor && same_rect(*previous_cursor, *current_cursor))) {
        return;
    }
    if (previous_cursor) {
        append_damage(damage, *previous_cursor);
    }
    if (current_cursor) {
        append_damage(damage, *current_cursor);
    }
}

struct SkiaPresenter::Impl {
    Impl(std::string requested_family, float requested_size, SkiaTheme requested_theme,
         SkiaFontSmoothing requested_smoothing)
        : family(std::move(requested_family)), size(requested_size), theme(requested_theme),
          smoothing(requested_smoothing) {
        manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
        if (!manager) {
            throw std::runtime_error("Skia Fontconfig manager is unavailable");
        }
        typeface = manager->matchFamilyStyle(family.c_str(), SkFontStyle{});
        if (!typeface) {
            typeface = manager->matchFamilyStyle(nullptr, SkFontStyle{});
        }
        if (!typeface) {
            throw std::runtime_error("Skia could not resolve a system typeface");
        }
        SkString resolved_name;
        typeface->getFamilyName(&resolved_name);
        resolved_family = resolved_name.c_str();
        font = make_font(typeface, size);
        label_font = make_font(typeface, size * 0.72F);
        shaper = SkShaper::Make(manager);
        if (!shaper) {
            throw std::runtime_error("Skia HarfBuzz shaper is unavailable");
        }

        SkFontMetrics metrics{};
        font.getMetrics(&metrics);
        ascent = metrics.fAscent;
        descent = metrics.fDescent;
        leading = metrics.fLeading;
        const SkScalar advance = font.measureText("M", 1, SkTextEncoding::kUTF8, nullptr, nullptr);
        cell_width = std::max(1, static_cast<int>(std::ceil(advance)));
        cell_height = std::max(
            1, static_cast<int>(std::ceil(metrics.fDescent - metrics.fAscent + metrics.fLeading)));
        SkFontMetrics label_metrics{};
        label_font.getMetrics(&label_metrics);
        label_height = label_metrics.fDescent - label_metrics.fAscent + label_metrics.fLeading;
    }

    SkFont make_font(const sk_sp<SkTypeface>& face, float font_size) const {
        SkFont result(face, font_size);
        switch (smoothing) {
        case SkiaFontSmoothing::Smooth:
            result.setEdging(SkFont::Edging::kAntiAlias);
            result.setHinting(SkFontHinting::kNone);
            result.setSubpixel(true);
            break;
        case SkiaFontSmoothing::Crisp:
            result.setEdging(SkFont::Edging::kAntiAlias);
            result.setHinting(SkFontHinting::kSlight);
            result.setForceAutoHinting(true);
            result.setSubpixel(true);
            break;
        case SkiaFontSmoothing::Sharp:
            result.setEdging(SkFont::Edging::kAntiAlias);
            result.setHinting(SkFontHinting::kFull);
            result.setSubpixel(false);
            break;
        case SkiaFontSmoothing::LcdSubpixel:
            result.setEdging(SkFont::Edging::kSubpixelAntiAlias);
            result.setSubpixel(true);
            break;
        }
        return result;
    }

    ui::SceneVerticalMetrics vertical_metrics(float viewport_height) const {
        return {.cell_height = static_cast<float>(cell_height),
                .viewport_height = viewport_height,
                .footer_heights = ui::editor_footer_heights(static_cast<float>(cell_height))};
    }

    sk_sp<SkTextBlob> shape_text(const std::string& text, const SkFont& text_font, SkScalar x,
                                 SkScalar y) {
        if (text.empty()) {
            return nullptr;
        }
        SkTextBlobBuilderRunHandler handler(text.c_str(), SkPoint::Make(x, y));
        shaper->shape(text.data(), text.size(), text_font, true, SK_ScalarMax, &handler);
        return handler.makeBlob();
    }

    sk_sp<SkTextBlob> shape_text(const std::string& text, SkScalar x, SkScalar y) {
        return shape_text(text, font, x, y);
    }

    float text_width(std::string_view text) const {
        return static_cast<float>(ui::display_width(text) * cell_width);
    }

    void draw_hairline(SkCanvas& canvas, const SkRect& rect) {
        SkPaint paint;
        paint.setAntiAlias(false);
        paint.setColor(color(theme.hairline));
        canvas.drawRect(rect, paint);
    }

    void paint_panel_shell(SkCanvas& canvas, const SkRect& panel) {
        const SkRRect panel_rrect = SkRRect::MakeRectXY(panel, panel_radius, panel_radius);
        SkPaint shadow;
        shadow.setAntiAlias(true);
        shadow.setColor(color(theme.shadow));
        shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 8.0F, false));
        SkRect shadow_rect = panel;
        shadow_rect.offset(0.0F, 4.0F);
        canvas.drawRRect(SkRRect::MakeRectXY(shadow_rect, panel_radius, panel_radius), shadow);

        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(color(theme.surface));
        canvas.drawRRect(panel_rrect, fill);

        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth(1.0F);
        stroke.setColor(color(theme.hairline));
        SkRect border = panel;
        border.inset(0.5F, 0.5F);
        canvas.drawRRect(SkRRect::MakeRectXY(border, panel_radius - 0.5F, panel_radius - 0.5F),
                         stroke);
    }

    void paint_popup(SkCanvas& canvas, const PopupLayout& layout, std::size_t region_index,
                     const RasterView& raster, SkiaRenderDiagnostics* diagnostics,
                     const SkRect* damage_bounds) {
        const ui::Region& region = *layout.region;
        const ui::Region::PopupContent& popup = *layout.content;
        if (damage_bounds && !SkRect::Intersects(layout.shadow, *damage_bounds)) {
            return;
        }

        paint_panel_shell(canvas, layout.panel);
        const SkRRect panel_rrect = SkRRect::MakeRectXY(layout.panel, panel_radius, panel_radius);

        const auto join_bounds = [](std::optional<SkRect>& bounds, const SkRect& incoming) {
            if (bounds) {
                bounds->join(incoming);
            } else {
                bounds = incoming;
            }
        };
        const auto draw_text = [&](std::string_view text, const SkFont& text_font, SkPoint origin,
                                   std::uint32_t text_color, std::optional<SkRect>& shape_bounds) {
            sk_sp<SkTextBlob> blob =
                shape_text(std::string(text), text_font, origin.x(), origin.y());
            if (!blob) {
                return;
            }
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(color(text_color));
            canvas.drawTextBlob(blob, 0.0F, 0.0F, paint);
            join_bounds(shape_bounds, blob->bounds());
        };
        const auto append_diagnostics = [&](std::size_t primitive_index, const SkRect& cell_bounds,
                                            const std::optional<SkRect>& shape_bounds,
                                            const std::optional<PixelProbe>& probe) {
            if (!diagnostics) {
                return;
            }
            SkiaPrimitiveRenderDiagnostics primitive{
                .region_index = region_index,
                .primitive_index = primitive_index,
                .layout_bounds = logical_rect(cell_bounds),
                .shape_bounds = std::nullopt,
                .paint_bounds = std::nullopt,
                .draw_bounds_cross_region_clip = false,
                .row_overflow = false,
                .column_overflow = false,
            };
            if (shape_bounds) {
                primitive.shape_bounds = logical_rect(*shape_bounds);
                primitive.draw_bounds_cross_region_clip =
                    extends_horizontally(*shape_bounds, layout.panel) ||
                    extends_vertically(*shape_bounds, layout.panel);
            }
            if (probe) {
                primitive.paint_bounds = changed_pixel_bounds(*probe, raster);
            }
            if (primitive.paint_bounds) {
                const SkRect painted =
                    SkRect::MakeXYWH(primitive.paint_bounds->x, primitive.paint_bounds->y,
                                     primitive.paint_bounds->width, primitive.paint_bounds->height);
                primitive.row_overflow = extends_vertically(painted, cell_bounds);
                primitive.column_overflow = extends_horizontally(painted, cell_bounds);
            }
            diagnostics->primitives.push_back(primitive);
        };

        canvas.save();
        canvas.clipRRect(panel_rrect, true);

        if (!region.prims.empty()) {
            std::optional<PixelProbe> probe;
            if (diagnostics) {
                probe = capture_probe(layout.header, raster);
            }
            std::optional<SkRect> shape_bounds;
            const float text_top =
                layout.header.top() +
                (layout.header.height() - static_cast<float>(cell_height)) * 0.5F;
            if (layout.input_active) {
                const std::string prompt = popup_prompt(popup);
                const std::string full_text = popup_input_text(popup, layout.input);
                const std::string count =
                    std::format("{}/{}", popup.selected_item.value_or(0) + 1, popup.total_items);
                canvas.save();
                canvas.clipRect(layout.header);
                float text_x = popup_text_left(layout, full_text, cell_width);
                draw_text(prompt, font, SkPoint::Make(text_x, text_top), theme.accent,
                          shape_bounds);
                text_x += text_width(prompt) + static_cast<float>(cell_width);
                draw_text(layout.input, font, SkPoint::Make(text_x, text_top), theme.text,
                          shape_bounds);
                draw_text(count, font,
                          SkPoint::Make(layout.header.right() - panel_padding_x - text_width(count),
                                        text_top),
                          theme.faint, shape_bounds);
                canvas.restore();
                draw_hairline(canvas, SkRect::MakeXYWH(layout.panel.left(), layout.header.bottom(),
                                                       layout.panel.width(), 1.0F));
            } else {
                std::string label = popup_label(popup.title);
                for (char& character : label) {
                    character =
                        static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
                }
                const float label_top =
                    layout.header.top() + (layout.header.height() - label_height) * 0.5F;
                draw_text(label, label_font,
                          SkPoint::Make(layout.header.left() + panel_padding_x, label_top),
                          theme.muted, shape_bounds);
            }
            append_diagnostics(0, layout.header, shape_bounds, probe);
        }

        for (std::size_t visible_index = 0; visible_index < layout.item_count; ++visible_index) {
            const std::size_t item_index = visible_index;
            const std::size_t primitive_index = item_index + 1;
            if (item_index >= popup.items.size() || primitive_index >= region.prims.size()) {
                break;
            }
            const ui::Region::PopupItem& item = popup.items[item_index];
            const ui::Prim& primitive = region.prims[primitive_index];
            const SkRect row = SkRect::MakeLTRB(
                layout.panel.left(),
                layout.rows_top + static_cast<float>(visible_index) * layout.row_height,
                layout.panel.right(),
                layout.rows_top + static_cast<float>(visible_index + 1) * layout.row_height);
            if (primitive.selected) {
                SkPaint fill;
                fill.setAntiAlias(false);
                fill.setColor(color(theme.raised));
                canvas.drawRect(row, fill);
            }

            std::optional<PixelProbe> probe;
            if (diagnostics) {
                probe = capture_probe(row, raster);
            }
            const float text_top =
                row.top() + (row.height() - static_cast<float>(cell_height)) * 0.5F;
            std::optional<SkRect> shape_bounds;
            canvas.save();
            canvas.clipRect(row);
            draw_text(item.label, font, SkPoint::Make(row.left() + panel_padding_x, text_top),
                      primitive.selected ? theme.strong : theme.text, shape_bounds);
            if (!item.detail.empty()) {
                const float detail_x = row.right() - panel_padding_x - text_width(item.detail);
                draw_text(item.detail, font, SkPoint::Make(detail_x, text_top), theme.muted,
                          shape_bounds);
            }
            canvas.restore();
            append_diagnostics(primitive_index, row, shape_bounds, probe);
        }

        canvas.restore();
    }

    void paint_status(SkCanvas& canvas, const ui::Region& region, const SkRect& bounds,
                      std::size_t region_index, const RasterView& raster,
                      SkiaRenderDiagnostics* diagnostics, const SkRect* damage_bounds) {
        if (damage_bounds && !SkRect::Intersects(bounds, *damage_bounds)) {
            return;
        }
        const ui::Region::StatusContent& status = region.status.value();

        canvas.save();
        canvas.clipRect(bounds);
        SkPaint fill;
        fill.setAntiAlias(false);
        fill.setColor(color(theme.surface));
        canvas.drawRect(bounds, fill);
        draw_hairline(canvas, SkRect::MakeXYWH(bounds.left(), bounds.top(), bounds.width(), 1.0F));

        std::optional<PixelProbe> probe;
        if (diagnostics) {
            probe = capture_probe(bounds, raster);
        }
        std::optional<SkRect> shape_bounds;
        const float text_top =
            bounds.top() + (bounds.height() - static_cast<float>(cell_height)) * 0.5F;
        const float center_y = bounds.centerY();
        const auto draw_text = [&](std::string_view text, float x, std::uint32_t text_color) {
            sk_sp<SkTextBlob> blob = shape_text(std::string(text), x, text_top);
            if (!blob) {
                return;
            }
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(color(text_color));
            canvas.drawTextBlob(blob, 0.0F, 0.0F, paint);
            if (shape_bounds) {
                shape_bounds->join(blob->bounds());
            } else {
                shape_bounds = blob->bounds();
            }
        };
        // The right group is placed first so the left group can clip against
        // it on narrow viewports.
        float right = bounds.right() - footer_padding_x;
        const auto draw_right = [&](std::string_view text, std::uint32_t text_color, float gap) {
            if (text.empty()) {
                return;
            }
            right -= text_width(text);
            draw_text(text, right, text_color);
            right -= gap;
        };
        if (show_debug_status) {
            draw_right(std::format("r{}", status.revision), theme.faint, footer_padding_x);
        }
        draw_right(status_percent(status), theme.muted, segment_gap);
        draw_right(std::format("{}:{}", status.line, status.column), theme.text, segment_gap);
        if (!status.style_origin.empty()) {
            draw_right("·", theme.faint, segment_gap);
            draw_right(status.style_origin, theme.muted, segment_gap);
        }
        if (!status.key.empty()) {
            const float chip_height = static_cast<float>(cell_height) + 4.0F;
            const float chip_width = text_width(status.key) + 14.0F;
            const SkRect chip = SkRect::MakeXYWH(right - chip_width, center_y - chip_height * 0.5F,
                                                 chip_width, chip_height);
            SkPaint chip_fill;
            chip_fill.setAntiAlias(true);
            chip_fill.setColor(color(theme.raised));
            canvas.drawRRect(SkRRect::MakeRectXY(chip, 5.0F, 5.0F), chip_fill);
            SkPaint chip_stroke;
            chip_stroke.setAntiAlias(true);
            chip_stroke.setStyle(SkPaint::kStroke_Style);
            chip_stroke.setStrokeWidth(1.0F);
            chip_stroke.setColor(color(theme.hairline));
            canvas.drawRRect(SkRRect::MakeRectXY(chip, 5.0F, 5.0F), chip_stroke);
            draw_text(status.key, chip.left() + 7.0F, theme.text);
            right = chip.left() - segment_gap;
        }

        canvas.save();
        canvas.clipRect(SkRect::MakeLTRB(bounds.left(), bounds.top(), right, bounds.bottom()));
        float x = bounds.left() + footer_padding_x;
        SkPaint dot;
        dot.setAntiAlias(true);
        if (status.dirty) {
            dot.setColor(color(theme.accent));
            canvas.drawCircle(x + 3.5F, center_y, 3.5F, dot);
        } else {
            dot.setStyle(SkPaint::kStroke_Style);
            dot.setStrokeWidth(1.5F);
            dot.setColor(color(theme.faint));
            canvas.drawCircle(x + 3.5F, center_y, 3.0F, dot);
        }
        x += 7.0F + segment_gap;
        const std::string name = status_basename(status.path);
        draw_text(name, x, theme.strong);
        x += text_width(name) + segment_gap;
        draw_text(status_directory(status.path), x, theme.muted);
        canvas.restore();
        canvas.restore();

        if (diagnostics) {
            SkiaPrimitiveRenderDiagnostics primitive{
                .region_index = region_index,
                .primitive_index = 0,
                .layout_bounds = logical_rect(bounds),
                .shape_bounds = std::nullopt,
                .paint_bounds = std::nullopt,
                .draw_bounds_cross_region_clip = false,
                .row_overflow = false,
                .column_overflow = false,
            };
            if (shape_bounds) {
                primitive.shape_bounds = logical_rect(*shape_bounds);
                primitive.draw_bounds_cross_region_clip =
                    extends_horizontally(*shape_bounds, bounds) ||
                    extends_vertically(*shape_bounds, bounds);
            }
            if (probe) {
                primitive.paint_bounds = changed_pixel_bounds(*probe, raster);
            }
            diagnostics->primitives.push_back(primitive);
        }
    }

    void render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                std::size_t row_bytes, float device_scale, SkiaRenderDiagnostics* diagnostics,
                std::span<const SkiaLogicalRect> damage, bool full_repaint,
                const SkiaAnimationFrame* animation) {
        if (!(device_scale > 0.0F)) {
            throw std::invalid_argument("SkiaPresenter received an invalid device scale");
        }
        const float viewport_height = static_cast<float>(pixel_height) / device_scale;
        const float viewport_width = static_cast<float>(pixel_width) / device_scale;
        const RasterView raster{.pixels = pixels,
                                .row_bytes = row_bytes,
                                .width = pixel_width,
                                .height = pixel_height,
                                .device_scale = device_scale};
        if (diagnostics && !full_repaint) {
            throw std::invalid_argument(
                "SkiaPresenter diagnostics require a full reference render");
        }
        if (diagnostics) {
            diagnostics->ascent = ascent;
            diagnostics->descent = descent;
            diagnostics->leading = leading;
            diagnostics->baseline_from_row_top = -ascent;
            diagnostics->primitives.clear();
            std::size_t primitive_count = 0;
            for (const ui::Region& region : scene.regions) {
                primitive_count += region.prims.size();
            }
            diagnostics->primitives.reserve(primitive_count);
        }
        const SkImageInfo info = SkImageInfo::MakeN32Premul(pixel_width, pixel_height);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, row_bytes);
        if (!surface) {
            throw std::invalid_argument("SkiaPresenter received an invalid pixel buffer");
        }
        SkCanvas& canvas = *surface->getCanvas();
        if (full_repaint) {
            canvas.clear(color(theme.canvas));
        }
        canvas.scale(device_scale, device_scale);

        const auto paint_layer = [&](const ui::Scene& layer_scene, float grid_offset_y,
                                     bool paint_grid, bool paint_footer,
                                     SkiaRenderDiagnostics* layer_diagnostics,
                                     const SkRect* damage_bounds) {
            const ui::SceneVerticalLayout vertical_layout(layer_scene,
                                                          vertical_metrics(viewport_height));
            const SkRect grid_clip =
                SkRect::MakeLTRB(0.0F, 0.0F, viewport_width, vertical_layout.grid_clip_bottom());
            const std::optional<PopupLayout> panel =
                popup_layout(layer_scene, {.width = viewport_width, .height = viewport_height},
                             vertical_metrics(viewport_height));
            SkPaint fill;
            fill.setAntiAlias(false);
            for (std::size_t region_index = 0; region_index < layer_scene.regions.size();
                 ++region_index) {
                const ui::Region& region = layer_scene.regions[region_index];
                const bool moving_grid = region.vertical_anchor == ui::VerticalAnchor::Grid;
                if ((moving_grid && !paint_grid) || (!moving_grid && !paint_footer)) {
                    continue;
                }
                if (panel && panel->region == &region) {
                    paint_popup(canvas, *panel, region_index, raster, layer_diagnostics,
                                damage_bounds);
                    continue;
                }
                SkRect bounds = region_pixel_rect(region, cell_width, cell_height, vertical_layout);
                if (moving_grid) {
                    bounds.offset(0.0F, grid_offset_y);
                }
                if (damage_bounds && !SkRect::Intersects(bounds, *damage_bounds)) {
                    continue;
                }
                if (region.role == ui::RegionRole::StatusBar && region.status) {
                    paint_status(canvas, region, bounds, region_index, raster, layer_diagnostics,
                                 damage_bounds);
                    continue;
                }
                fill.setColor(color(region.surface == ui::SurfaceClass::Status ? theme.surface
                                                                               : theme.canvas));
                canvas.save();
                if (moving_grid) {
                    canvas.clipRect(grid_clip);
                }
                canvas.clipRect(bounds);
                canvas.drawRect(bounds, fill);
                if (layer_scene.active_text_row && moving_grid &&
                    (region.role == ui::RegionRole::TextArea ||
                     region.role == ui::RegionRole::LineNumbers ||
                     region.role == ui::RegionRole::ChangeSigns)) {
                    const float active_top =
                        bounds.top() +
                        static_cast<float>(*layer_scene.active_text_row - region.rect.row) *
                            static_cast<float>(cell_height);
                    fill.setColor(color(theme.active_line));
                    canvas.drawRect(SkRect::MakeXYWH(bounds.left(), active_top, bounds.width(),
                                                     static_cast<float>(cell_height)),
                                    fill);
                }
                if (region.role == ui::RegionRole::StatusBar) {
                    draw_hairline(canvas, SkRect::MakeXYWH(bounds.left(), bounds.top(),
                                                           bounds.width(), 1.0F));
                }
                const bool footer_region = region.vertical_anchor == ui::VerticalAnchor::Bottom;
                const bool suppress_echo_text =
                    panel && panel->input_active && region.role == ui::RegionRole::EchoArea;
                for (std::size_t primitive_index = 0; primitive_index < region.prims.size();
                     ++primitive_index) {
                    if (suppress_echo_text) {
                        continue;
                    }
                    const ui::Prim& prim = region.prims[primitive_index];
                    SkScalar x = bounds.left() + static_cast<SkScalar>(prim.col * cell_width);
                    SkScalar top = bounds.top() + static_cast<SkScalar>(prim.row * cell_height);
                    if (footer_region) {
                        const int scene_row = region.rect.row + prim.row;
                        top = vertical_layout.row_top(scene_row) +
                              (vertical_layout.row_height(scene_row) -
                               static_cast<float>(cell_height)) *
                                  0.5F;
                        if (region.role == ui::RegionRole::EchoArea) {
                            x += footer_padding_x;
                        }
                    }
                    const SkRect cell_bounds = SkRect::MakeXYWH(
                        x, top,
                        static_cast<SkScalar>(std::max(1, ui::display_width(prim.text)) *
                                              cell_width),
                        static_cast<SkScalar>(cell_height));
                    if (damage_bounds && !SkRect::Intersects(cell_bounds, *damage_bounds)) {
                        continue;
                    }

                    if (prim.selected) {
                        fill.setColor(color(theme.selection));
                        canvas.drawRect(
                            SkRect::MakeXYWH(
                                x, top,
                                static_cast<SkScalar>(ui::display_width(prim.text) * cell_width),
                                static_cast<SkScalar>(cell_height)),
                            fill);
                    }

                    SkPaint paint;
                    paint.setAntiAlias(true);
                    const bool active_line_number =
                        layer_scene.active_text_row && region.role == ui::RegionRole::LineNumbers &&
                        prim.row == *layer_scene.active_text_row - region.rect.row;
                    paint.setColor(active_line_number ? color(theme.text)
                                                      : foreground(prim.style, theme));
                    std::optional<SkRect> draw_bounds;
                    std::optional<SkRect> shape_bounds;
                    sk_sp<SkTextBlob> blob;
                    if (prim.kind != ui::PrimKind::Text) {
                        if (prim.kind == ui::PrimKind::ChangeDeletion) {
                            draw_bounds = SkRect::MakeXYWH(
                                x + 1.0F, top + 1.0F, static_cast<SkScalar>(cell_width - 2), 2.0F);
                        } else {
                            draw_bounds = SkRect::MakeXYWH(x + 1.0F, top + 2.0F, 3.0F,
                                                           static_cast<SkScalar>(cell_height - 4));
                        }
                    } else {
                        blob = shape_text(prim.text, x, top);
                        if (blob) {
                            shape_bounds = blob->bounds();
                            draw_bounds = shape_bounds;
                        }
                    }

                    std::optional<PixelProbe> probe;
                    if (layer_diagnostics && draw_bounds) {
                        SkRect probe_bounds = cell_bounds;
                        probe_bounds.join(*draw_bounds);
                        probe = capture_probe(probe_bounds, raster);
                    }
                    if (prim.kind != ui::PrimKind::Text && draw_bounds) {
                        if (prim.kind == ui::PrimKind::ChangeBar) {
                            SkPaint bar;
                            bar.setAntiAlias(true);
                            bar.setColor(paint.getColor());
                            canvas.drawRRect(SkRRect::MakeRectXY(*draw_bounds, 1.5F, 1.5F), bar);
                        } else {
                            canvas.drawRect(*draw_bounds, paint);
                        }
                    } else if (blob) {
                        canvas.drawTextBlob(blob, 0.0F, 0.0F, paint);
                    }

                    if (layer_diagnostics) {
                        SkiaPrimitiveRenderDiagnostics primitive{
                            .region_index = region_index,
                            .primitive_index = primitive_index,
                            .layout_bounds = logical_rect(cell_bounds),
                            .shape_bounds = std::nullopt,
                            .paint_bounds = std::nullopt,
                            .draw_bounds_cross_region_clip = false,
                            .row_overflow = false,
                            .column_overflow = false,
                        };
                        if (shape_bounds) {
                            primitive.shape_bounds = logical_rect(*shape_bounds);
                        }
                        if (draw_bounds) {
                            primitive.draw_bounds_cross_region_clip =
                                extends_horizontally(*draw_bounds, bounds) ||
                                extends_vertically(*draw_bounds, bounds);
                        }
                        if (probe) {
                            primitive.paint_bounds = changed_pixel_bounds(*probe, raster);
                        }
                        if (primitive.paint_bounds) {
                            const SkRect raster_bounds = SkRect::MakeXYWH(
                                primitive.paint_bounds->x, primitive.paint_bounds->y,
                                primitive.paint_bounds->width, primitive.paint_bounds->height);
                            primitive.row_overflow = extends_vertically(raster_bounds, cell_bounds);
                            primitive.column_overflow =
                                extends_horizontally(raster_bounds, cell_bounds);
                        }
                        layer_diagnostics->primitives.push_back(primitive);
                    }
                }
                canvas.restore();
            }
        };

        const auto paint_cursor = [&](float grid_offset_y,
                                      const std::optional<SkiaLogicalPoint>& position,
                                      const SkRect* damage_bounds) {
            if (!scene.cursor_visible) {
                return;
            }
            const ui::SceneVerticalLayout vertical_layout(scene, vertical_metrics(viewport_height));
            const int cursor_row = std::max(0, scene.cursor_row - 1);
            const int cursor_col = std::max(0, scene.cursor_col - 1);
            std::optional<SkRect> cursor_clip;
            bool cursor_in_footer = false;
            bool cursor_in_echo = false;
            const std::optional<PopupLayout> panel =
                popup_layout(scene, {.width = viewport_width, .height = viewport_height},
                             vertical_metrics(viewport_height));
            const std::optional<SkRect> popup_cursor =
                panel ? popup_cursor_rect(*panel, {.width = cell_width, .height = cell_height})
                      : std::nullopt;
            if (popup_cursor) {
                cursor_in_footer = true;
                cursor_clip = panel->header;
            } else {
                for (const ui::Region& region : scene.regions) {
                    if (contains(region.rect, cursor_row, cursor_col)) {
                        cursor_in_footer = region.vertical_anchor != ui::VerticalAnchor::Grid;
                        cursor_in_echo = cursor_in_footer &&
                                         region.vertical_anchor == ui::VerticalAnchor::Bottom &&
                                         region.role == ui::RegionRole::EchoArea;
                        cursor_clip =
                            region_pixel_rect(region, cell_width, cell_height, vertical_layout);
                        if (!cursor_in_footer) {
                            cursor_clip->offset(0.0F, grid_offset_y);
                        }
                        break;
                    }
                }
            }
            SkScalar cursor_x = static_cast<SkScalar>(cursor_col * cell_width);
            SkScalar cursor_y = vertical_layout.row_top(cursor_row);
            if (cursor_in_footer && !popup_cursor) {
                cursor_y +=
                    (vertical_layout.row_height(cursor_row) - static_cast<float>(cell_height)) *
                    0.5F;
                if (cursor_in_echo) {
                    cursor_x += footer_padding_x;
                }
            } else if (!cursor_in_footer) {
                cursor_y += grid_offset_y;
            }
            if (popup_cursor) {
                cursor_x = popup_cursor->left();
                cursor_y = popup_cursor->top();
            }
            if (position) {
                cursor_x = position->x;
                cursor_y = position->y;
            }
            const SkRect cursor_bounds =
                SkRect::MakeXYWH(cursor_x, cursor_y, 2.0F, static_cast<SkScalar>(cell_height));
            if (damage_bounds && !SkRect::Intersects(cursor_bounds, *damage_bounds)) {
                return;
            }
            SkPaint fill;
            fill.setAntiAlias(false);
            fill.setColor(color(theme.cursor));
            canvas.save();
            if (!cursor_in_footer) {
                canvas.clipRect(SkRect::MakeLTRB(0.0F, 0.0F, viewport_width,
                                                 vertical_layout.grid_clip_bottom()));
            }
            if (cursor_clip) {
                canvas.clipRect(*cursor_clip);
            }
            canvas.drawRect(cursor_bounds, fill);
            canvas.restore();
        };

        if (full_repaint) {
            if (animation && animation->scroll_source) {
                paint_layer(*animation->scroll_source, animation->source_grid_offset_y, true, false,
                            nullptr, nullptr);
                paint_layer(scene, animation->target_grid_offset_y, true, false, diagnostics,
                            nullptr);
                paint_layer(scene, 0.0F, false, true, diagnostics, nullptr);
            } else {
                paint_layer(scene, 0.0F, true, true, diagnostics, nullptr);
            }
            paint_cursor(animation ? animation->target_grid_offset_y : 0.0F,
                         animation ? animation->cursor_position : std::optional<SkiaLogicalPoint>{},
                         nullptr);
            return;
        }
        SkPaint clear;
        clear.setAntiAlias(false);
        clear.setColor(color(theme.canvas));
        for (const SkiaLogicalRect& logical : damage) {
            const SkRect bounds =
                SkRect::MakeXYWH(logical.x, logical.y, logical.width, logical.height);
            if (bounds.isEmpty()) {
                continue;
            }
            canvas.save();
            canvas.clipRect(bounds);
            canvas.drawRect(bounds, clear);
            paint_layer(scene, 0.0F, true, true, nullptr, &bounds);
            paint_cursor(0.0F, std::nullopt, &bounds);
            canvas.restore();
        }
    }

    std::string family;
    std::string resolved_family;
    float size;
    SkiaTheme theme;
    sk_sp<SkFontMgr> manager;
    sk_sp<SkTypeface> typeface;
    SkFont font;
    SkFont label_font;
    std::unique_ptr<SkShaper> shaper;
    SkiaFontSmoothing smoothing = SkiaFontSmoothing::Smooth;
    float ascent = 0.0F;
    float descent = 0.0F;
    float leading = 0.0F;
    float label_height = 0.0F;
    int cell_width = 1;
    int cell_height = 1;
    bool show_debug_status = false;
};

SkiaFontSmoothing parse_font_smoothing(std::string_view name) {
    if (name == "crisp") {
        return SkiaFontSmoothing::Crisp;
    }
    if (name == "sharp" || name == "win") {
        return SkiaFontSmoothing::Sharp;
    }
    if (name == "lcd" || name == "subpixel" || name == "legacy") {
        return SkiaFontSmoothing::LcdSubpixel;
    }
    return SkiaFontSmoothing::Smooth;
}

SkiaPresenter::SkiaPresenter(std::string font_family, float font_size, SkiaTheme theme,
                             SkiaFontSmoothing smoothing)
    : impl_(std::make_unique<Impl>(std::move(font_family), font_size, theme, smoothing)) {}

SkiaPresenter::~SkiaPresenter() = default;
SkiaPresenter::SkiaPresenter(SkiaPresenter&&) noexcept = default;
SkiaPresenter& SkiaPresenter::operator=(SkiaPresenter&&) noexcept = default;

int SkiaPresenter::cell_width() const {
    return impl_->cell_width;
}
int SkiaPresenter::cell_height() const {
    return impl_->cell_height;
}

const std::string& SkiaPresenter::font_family() const {
    return impl_->resolved_family;
}

float SkiaPresenter::font_size() const {
    return impl_->size;
}

const SkiaTheme& SkiaPresenter::theme() const {
    return impl_->theme;
}

float SkiaPresenter::status_bar_height() const {
    return footer_height_for(ui::RegionRole::StatusBar, static_cast<float>(impl_->cell_height));
}

float SkiaPresenter::echo_area_height() const {
    return footer_height_for(ui::RegionRole::EchoArea, static_cast<float>(impl_->cell_height));
}

ui::SceneVerticalMetrics SkiaPresenter::vertical_metrics(float viewport_height) const {
    return impl_->vertical_metrics(viewport_height);
}

void SkiaPresenter::set_show_debug_status(bool show) {
    impl_->show_debug_status = show;
}

std::optional<SkiaLogicalRect> SkiaPresenter::cursor_rect(const ui::Scene& scene,
                                                          float viewport_width,
                                                          float viewport_height) const {
    if (!scene.cursor_visible) {
        return std::nullopt;
    }
    if (const std::optional<PopupLayout> panel =
            popup_layout(scene, {.width = viewport_width, .height = viewport_height},
                         impl_->vertical_metrics(viewport_height))) {
        if (const std::optional<SkRect> cursor = popup_cursor_rect(
                *panel, {.width = impl_->cell_width, .height = impl_->cell_height})) {
            return logical_rect(*cursor);
        }
    }
    const ui::SceneVerticalLayout vertical_layout(scene, impl_->vertical_metrics(viewport_height));
    const int row = std::max(0, scene.cursor_row - 1);
    const int column = std::max(0, scene.cursor_col - 1);
    float x = static_cast<float>(column * impl_->cell_width);
    float y = vertical_layout.row_top(row);
    const std::optional<int>& anchor = vertical_layout.bottom_anchor_row();
    if (anchor && row >= *anchor) {
        y += (vertical_layout.row_height(row) - static_cast<float>(impl_->cell_height)) * 0.5F;
        for (const ui::Region& region : scene.regions) {
            if (region.role == ui::RegionRole::EchoArea &&
                region.vertical_anchor == ui::VerticalAnchor::Bottom &&
                contains(region.rect, row, column)) {
                x += footer_padding_x;
                break;
            }
        }
    }
    return SkiaLogicalRect{
        .x = x,
        .y = y,
        .width = 2.0F,
        .height = static_cast<float>(impl_->cell_height),
    };
}

std::vector<SkiaLogicalRect> SkiaPresenter::damage_rects(const ui::Scene& scene,
                                                         const ui::SceneDamage& damage,
                                                         float viewport_width,
                                                         float viewport_height) const {
    const float frame_width = std::max(0.0F, viewport_width);
    const float frame_height = std::max(0.0F, viewport_height);
    if (damage.full_repaint) {
        return {{.x = 0.0F, .y = 0.0F, .width = frame_width, .height = frame_height}};
    }

    const ui::SceneVerticalLayout vertical_layout(scene, impl_->vertical_metrics(frame_height));
    const std::optional<int>& anchor_row = vertical_layout.bottom_anchor_row();
    const float cell_width = static_cast<float>(impl_->cell_width);
    const float cell_height = static_cast<float>(impl_->cell_height);
    const float footer_top = vertical_layout.grid_clip_bottom();
    std::vector<SkiaLogicalRect> rectangles;
    rectangles.reserve(damage.cell_rects.size() + damage.cursor_cells.size());
    const auto append_cells = [&](const ui::Rect& cells, float vertical_limit) {
        const float left = static_cast<float>(cells.col * impl_->cell_width);
        const float top = vertical_layout.row_top(cells.row);
        const float right = static_cast<float>((cells.col + cells.cols) * impl_->cell_width);
        const float bottom = vertical_layout.row_top(cells.row + cells.rows);
        const float expanded_left = std::max(0.0F, left - 2.0F * cell_width);
        const float expanded_top = std::max(0.0F, top - cell_height);
        const float expanded_right = std::min(frame_width, right + 2.0F * cell_width);
        const float expanded_bottom =
            std::min({frame_height, vertical_limit, bottom + cell_height});
        if (expanded_right <= expanded_left || expanded_bottom <= expanded_top) {
            return;
        }
        append_damage(rectangles, {.x = expanded_left,
                                   .y = expanded_top,
                                   .width = expanded_right - expanded_left,
                                   .height = expanded_bottom - expanded_top});
    };
    for (const ui::Rect& cells : damage.cell_rects) {
        const int last_row = cells.row + cells.rows;
        if (!anchor_row || cells.row >= *anchor_row || last_row <= *anchor_row) {
            append_cells(cells, anchor_row && last_row <= *anchor_row ? footer_top : frame_height);
            continue;
        }
        append_cells({.row = cells.row,
                      .col = cells.col,
                      .rows = *anchor_row - cells.row,
                      .cols = cells.cols},
                     footer_top);
        append_cells({.row = *anchor_row,
                      .col = cells.col,
                      .rows = last_row - *anchor_row,
                      .cols = cells.cols},
                     frame_height);
    }
    for (const ui::Rect& cells : damage.cell_rects) {
        for (const ui::Region& region : scene.regions) {
            if (region.vertical_anchor != ui::VerticalAnchor::Overlay) {
                continue;
            }
            const int first_row = std::max(cells.row, region.rect.row);
            const int last_row =
                std::min(cells.row + cells.rows, region.rect.row + region.rect.rows);
            const int first_col = std::max(cells.col, region.rect.col);
            const int last_col =
                std::min(cells.col + cells.cols, region.rect.col + region.rect.cols);
            if (first_row >= last_row || first_col >= last_col) {
                continue;
            }
            const float left = static_cast<float>(first_col * impl_->cell_width);
            const float top = static_cast<float>(first_row * impl_->cell_height);
            const float right = static_cast<float>(last_col * impl_->cell_width);
            const float bottom = static_cast<float>(last_row * impl_->cell_height);
            const float expanded_left = std::max(0.0F, left - 2.0F * cell_width);
            const float expanded_top = std::max(0.0F, top - cell_height);
            const float expanded_right = std::min(frame_width, right + 2.0F * cell_width);
            const float expanded_bottom = std::min(frame_height, bottom + cell_height);
            if (expanded_right > expanded_left && expanded_bottom > expanded_top) {
                append_damage(rectangles, {.x = expanded_left,
                                           .y = expanded_top,
                                           .width = expanded_right - expanded_left,
                                           .height = expanded_bottom - expanded_top});
            }
        }
    }
    // The modeline lays segments out independently of the cell grid, so any
    // damaged status cell repaints the whole bar.
    if (const ui::Region* status_region = scene.find(ui::RegionRole::StatusBar);
        status_region && status_region->status) {
        const int first = status_region->rect.row;
        const int last = first + status_region->rect.rows;
        const bool touched = std::ranges::any_of(damage.cell_rects, [&](const ui::Rect& cells) {
            return cells.row < last && cells.row + cells.rows > first;
        });
        if (touched) {
            const float top = std::max(0.0F, vertical_layout.row_top(first));
            const float bottom = std::min(frame_height, vertical_layout.row_top(last));
            if (bottom > top) {
                append_damage(rectangles,
                              {.x = 0.0F, .y = top, .width = frame_width, .height = bottom - top});
            }
        }
    }
    if ((!damage.cell_rects.empty() || !damage.cursor_cells.empty())) {
        if (const std::optional<PopupLayout> panel =
                popup_layout(scene, {.width = frame_width, .height = frame_height},
                             impl_->vertical_metrics(frame_height))) {
            append_damage(rectangles, logical_rect(panel->shadow));
        }
    }
    for (const ui::CellPoint& cursor : damage.cursor_cells) {
        const float top = vertical_layout.row_top(cursor.row);
        const bool footer_cursor = anchor_row && cursor.row >= *anchor_row;
        const float bottom = anchor_row && cursor.row < *anchor_row
                                 ? std::min(footer_top, top + cell_height)
                                 : std::min(frame_height, vertical_layout.row_top(cursor.row + 1));
        if (bottom <= top) {
            continue;
        }
        const float left =
            std::clamp(static_cast<float>(cursor.column * impl_->cell_width), 0.0F, frame_width);
        const float right =
            std::min(frame_width, left + 2.0F + (footer_cursor ? footer_padding_x : 0.0F));
        if (right > left) {
            append_damage(rectangles,
                          {.x = left, .y = top, .width = right - left, .height = bottom - top});
        }
    }
    return rectangles;
}

void SkiaPresenter::render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                           std::size_t row_bytes, float device_scale,
                           SkiaRenderDiagnostics* diagnostics) {
    impl_->render(scene, pixel_width, pixel_height, pixels, row_bytes, device_scale, diagnostics,
                  std::span<const SkiaLogicalRect>{}, true, nullptr);
}

void SkiaPresenter::render_damage(const ui::Scene& scene, int pixel_width, int pixel_height,
                                  void* pixels, std::size_t row_bytes,
                                  std::span<const SkiaLogicalRect> damage, float device_scale) {
    impl_->render(scene, pixel_width, pixel_height, pixels, row_bytes, device_scale, nullptr,
                  damage, false, nullptr);
}

void SkiaPresenter::render_animated(const ui::Scene& scene, const SkiaAnimationFrame& animation,
                                    int pixel_width, int pixel_height, void* pixels,
                                    std::size_t row_bytes, float device_scale,
                                    SkiaRenderDiagnostics* diagnostics) {
    impl_->render(scene, pixel_width, pixel_height, pixels, row_bytes, device_scale, diagnostics,
                  std::span<const SkiaLogicalRect>{}, true, &animation);
}

} // namespace cind::gui
