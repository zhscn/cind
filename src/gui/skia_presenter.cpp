#include "gui/skia_presenter.hpp"

#include "ui/char_width.hpp"
#include "ui/scene_layout.hpp"

#include <modules/skshaper/include/SkShaper.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColor.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMetrics.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkFontStyle.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkRect.h>
#include <skia/core/SkString.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkTextBlob.h>
#include <skia/core/SkTypeface.h>
#include <skia/ports/SkFontMgr_fontconfig.h>
#include <skia/ports/SkFontScanner_FreeType.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace cind::gui {

namespace {

SkColor color(std::uint32_t argb) {
    return static_cast<SkColor>(argb);
}

SkColor foreground(ui::StyleClass style, const SkiaTheme& theme) {
    switch (style) {
    case ui::StyleClass::Text:
        return color(0xFFD4D4D4);
    case ui::StyleClass::Keyword:
        return color(0xFF569CD6);
    case ui::StyleClass::String:
        return color(0xFF6A9955);
    case ui::StyleClass::Number:
        return color(0xFFB5CEA8);
    case ui::StyleClass::Comment:
        return color(0xFF808080);
    case ui::StyleClass::Preprocessor:
        return color(0xFFDCDCAA);
    case ui::StyleClass::Gutter:
        return color(0xFF858585);
    case ui::StyleClass::SignAdded:
        return color(theme.sign_added);
    case ui::StyleClass::SignModified:
        return color(theme.sign_modified);
    case ui::StyleClass::SignDeleted:
        return color(theme.sign_deleted);
    case ui::StyleClass::StatusBar:
        return color(0xFFF0F0F0);
    case ui::StyleClass::StatusKey:
        return color(0xFFFFFFFF);
    case ui::StyleClass::Message:
        return color(0xFFD4D4D4);
    }
    return color(0xFFD4D4D4);
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
    if (region.vertical_anchor == ui::VerticalAnchor::Bottom) {
        bounds.offset(0.0F, layout.row_top(region.rect.row) -
                                static_cast<float>(region.rect.row * cell_height));
    } else if (layout.bottom_anchor_row()) {
        const float footer_top = layout.grid_clip_bottom();
        bounds = SkRect::MakeLTRB(bounds.left(), bounds.top(), bounds.right(),
                                  std::max(bounds.top(), std::min(bounds.bottom(), footer_top)));
    }
    return bounds;
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

} // namespace

struct SkiaPresenter::Impl {
    Impl(std::string requested_family, float requested_size, SkiaTheme requested_theme)
        : family(std::move(requested_family)), size(requested_size), theme(requested_theme) {
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
        font = make_font(typeface);
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
    }

    SkFont make_font(const sk_sp<SkTypeface>& face) const {
        SkFont result(face, size);
        result.setEdging(SkFont::Edging::kSubpixelAntiAlias);
        result.setSubpixel(true);
        return result;
    }

    sk_sp<SkTextBlob> shape_text(const std::string& text, SkScalar x, SkScalar y) {
        if (text.empty()) {
            return nullptr;
        }
        SkTextBlobBuilderRunHandler handler(text.c_str(), SkPoint::Make(x, y));
        shaper->shape(text.data(), text.size(), font, true, SK_ScalarMax, &handler);
        return handler.makeBlob();
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
            canvas.clear(color(theme.background));
        }
        canvas.scale(device_scale, device_scale);

        const auto paint_layer = [&](const ui::Scene& layer_scene, float grid_offset_y,
                                     bool paint_grid, bool paint_footer,
                                     SkiaRenderDiagnostics* layer_diagnostics,
                                     const SkRect* damage_bounds) {
            const ui::SceneVerticalLayout vertical_layout(
                layer_scene, {.cell_height = static_cast<float>(cell_height),
                              .viewport_height = viewport_height});
            const SkRect grid_clip =
                SkRect::MakeLTRB(0.0F, 0.0F, viewport_width, vertical_layout.grid_clip_bottom());
            SkPaint fill;
            fill.setAntiAlias(false);
            for (std::size_t region_index = 0; region_index < layer_scene.regions.size();
                 ++region_index) {
                const ui::Region& region = layer_scene.regions[region_index];
                const bool footer = region.vertical_anchor == ui::VerticalAnchor::Bottom;
                if ((footer && !paint_footer) || (!footer && !paint_grid)) {
                    continue;
                }
                SkRect bounds = region_pixel_rect(region, cell_width, cell_height, vertical_layout);
                if (!footer) {
                    bounds.offset(0.0F, grid_offset_y);
                }
                if (damage_bounds && !SkRect::Intersects(bounds, *damage_bounds)) {
                    continue;
                }
                switch (region.surface) {
                case ui::SurfaceClass::Gutter:
                    fill.setColor(color(theme.gutter_background));
                    break;
                case ui::SurfaceClass::Status:
                    fill.setColor(color(theme.status_background));
                    break;
                case ui::SurfaceClass::Echo:
                    fill.setColor(color(theme.echo_background));
                    break;
                case ui::SurfaceClass::Editor:
                    fill.setColor(color(theme.background));
                    break;
                }
                canvas.save();
                if (!footer) {
                    canvas.clipRect(grid_clip);
                }
                canvas.clipRect(bounds);
                canvas.drawRect(bounds, fill);
                for (std::size_t primitive_index = 0; primitive_index < region.prims.size();
                     ++primitive_index) {
                    const ui::Prim& prim = region.prims[primitive_index];
                    const SkScalar x = bounds.left() + static_cast<SkScalar>(prim.col * cell_width);
                    const SkScalar top =
                        bounds.top() + static_cast<SkScalar>(prim.row * cell_height);
                    const SkRect cell_bounds = SkRect::MakeXYWH(
                        x, top,
                        static_cast<SkScalar>(std::max(1, ui::display_width(prim.text)) *
                                              cell_width),
                        static_cast<SkScalar>(cell_height));
                    if (damage_bounds && !SkRect::Intersects(cell_bounds, *damage_bounds)) {
                        continue;
                    }

                    if (prim.selected) {
                        fill.setColor(color(theme.selection_background));
                        canvas.drawRect(
                            SkRect::MakeXYWH(
                                x, top,
                                static_cast<SkScalar>(ui::display_width(prim.text) * cell_width),
                                static_cast<SkScalar>(cell_height)),
                            fill);
                    }

                    SkPaint paint;
                    paint.setAntiAlias(true);
                    paint.setColor(foreground(prim.style, theme));
                    std::optional<SkRect> draw_bounds;
                    std::optional<SkRect> shape_bounds;
                    sk_sp<SkTextBlob> blob;
                    if (prim.kind != ui::PrimKind::Text) {
                        if (prim.kind == ui::PrimKind::ChangeDeletion) {
                            draw_bounds = SkRect::MakeXYWH(
                                x + 1.0F, top + 1.0F, static_cast<SkScalar>(cell_width - 2), 2.0F);
                        } else {
                            draw_bounds = SkRect::MakeXYWH(x + 1.0F, top + 1.0F, 3.0F,
                                                           static_cast<SkScalar>(cell_height - 2));
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
                        canvas.drawRect(*draw_bounds, paint);
                    } else if (blob) {
                        canvas.drawTextBlob(blob, 0.0F, 0.0F, paint);
                    }

                    if (layer_diagnostics) {
                        SkiaPrimitiveRenderDiagnostics primitive{
                            .region_index = region_index,
                            .primitive_index = primitive_index,
                            .cell_bounds = logical_rect(cell_bounds),
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
            const ui::SceneVerticalLayout vertical_layout(
                scene, {.cell_height = static_cast<float>(cell_height),
                        .viewport_height = viewport_height});
            const int cursor_row = std::max(0, scene.cursor_row - 1);
            const int cursor_col = std::max(0, scene.cursor_col - 1);
            std::optional<SkRect> cursor_clip;
            bool cursor_in_footer = false;
            for (const ui::Region& region : scene.regions) {
                if (contains(region.rect, cursor_row, cursor_col)) {
                    cursor_in_footer = region.vertical_anchor == ui::VerticalAnchor::Bottom;
                    cursor_clip =
                        region_pixel_rect(region, cell_width, cell_height, vertical_layout);
                    if (!cursor_in_footer) {
                        cursor_clip->offset(0.0F, grid_offset_y);
                    }
                    break;
                }
            }
            const SkScalar cursor_x =
                position ? position->x : static_cast<SkScalar>(cursor_col * cell_width);
            const SkScalar cursor_y = position ? position->y
                                               : vertical_layout.row_top(cursor_row) +
                                                     (cursor_in_footer ? 0.0F : grid_offset_y);
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
        clear.setColor(color(theme.background));
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
    std::unique_ptr<SkShaper> shaper;
    float ascent = 0.0F;
    float descent = 0.0F;
    float leading = 0.0F;
    int cell_width = 1;
    int cell_height = 1;
};

SkiaPresenter::SkiaPresenter(std::string font_family, float font_size, SkiaTheme theme)
    : impl_(std::make_unique<Impl>(std::move(font_family), font_size, theme)) {}

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

std::vector<SkiaLogicalRect> SkiaPresenter::damage_rects(const ui::Scene& scene,
                                                         const ui::SceneDamage& damage,
                                                         float viewport_width,
                                                         float viewport_height) const {
    const float frame_width = std::max(0.0F, viewport_width);
    const float frame_height = std::max(0.0F, viewport_height);
    if (damage.full_repaint) {
        return {{.x = 0.0F, .y = 0.0F, .width = frame_width, .height = frame_height}};
    }

    const ui::SceneVerticalLayout vertical_layout(
        scene,
        {.cell_height = static_cast<float>(impl_->cell_height), .viewport_height = frame_height});
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
    for (const ui::CellPoint& cursor : damage.cursor_cells) {
        const float top = vertical_layout.row_top(cursor.row);
        const float bottom = anchor_row && cursor.row < *anchor_row
                                 ? std::min(footer_top, top + cell_height)
                                 : std::min(frame_height, top + cell_height);
        if (bottom <= top) {
            continue;
        }
        const float left =
            std::clamp(static_cast<float>(cursor.column * impl_->cell_width), 0.0F, frame_width);
        const float right = std::min(frame_width, left + 2.0F);
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
