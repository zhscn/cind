#include "gui/skia_presenter.hpp"

#include "ui/char_width.hpp"

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
#include <string_view>
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

struct PixelProbe {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    std::vector<std::uint32_t> before;
};

std::uint32_t read_pixel(const void* pixels, std::size_t row_bytes, int x, int y) {
    const auto* address = static_cast<const std::byte*>(pixels) +
                          static_cast<std::size_t>(y) * row_bytes +
                          static_cast<std::size_t>(x) * sizeof(std::uint32_t);
    std::uint32_t pixel = 0;
    std::memcpy(&pixel, address, sizeof(pixel));
    return pixel;
}

PixelProbe capture_probe(const SkRect& logical_bounds, const void* pixels, std::size_t row_bytes,
                         int pixel_width, int pixel_height, float device_scale) {
    PixelProbe probe{
        .left = std::clamp(static_cast<int>(std::floor(logical_bounds.left() * device_scale)) - 1,
                           0, pixel_width),
        .top = std::clamp(static_cast<int>(std::floor(logical_bounds.top() * device_scale)) - 1, 0,
                          pixel_height),
        .right = std::clamp(static_cast<int>(std::ceil(logical_bounds.right() * device_scale)) + 1,
                            0, pixel_width),
        .bottom =
            std::clamp(static_cast<int>(std::ceil(logical_bounds.bottom() * device_scale)) + 1, 0,
                       pixel_height),
        .before = {},
    };
    probe.before.reserve(static_cast<std::size_t>(probe.right - probe.left) *
                         static_cast<std::size_t>(probe.bottom - probe.top));
    for (int y = probe.top; y < probe.bottom; ++y) {
        for (int x = probe.left; x < probe.right; ++x) {
            probe.before.push_back(read_pixel(pixels, row_bytes, x, y));
        }
    }
    return probe;
}

std::optional<SkiaLogicalRect> changed_pixel_bounds(const PixelProbe& probe, const void* pixels,
                                                    std::size_t row_bytes, float device_scale) {
    int left = probe.right;
    int top = probe.bottom;
    int right = probe.left;
    int bottom = probe.top;
    std::size_t index = 0;
    for (int y = probe.top; y < probe.bottom; ++y) {
        for (int x = probe.left; x < probe.right; ++x, ++index) {
            if (read_pixel(pixels, row_bytes, x, y) == probe.before[index]) {
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
    return SkiaLogicalRect{.x = static_cast<float>(left) / device_scale,
                           .y = static_cast<float>(top) / device_scale,
                           .width = static_cast<float>(right - left) / device_scale,
                           .height = static_cast<float>(bottom - top) / device_scale};
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

    sk_sp<SkTextBlob> shape_text(std::string_view text, SkScalar x, SkScalar y) {
        if (text.empty()) {
            return nullptr;
        }
        SkTextBlobBuilderRunHandler handler(text.data(), SkPoint::Make(x, y));
        shaper->shape(text.data(), text.size(), font, true, SK_ScalarMax, &handler);
        return handler.makeBlob();
    }

    void render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                std::size_t row_bytes, float device_scale, SkiaRenderDiagnostics* diagnostics) {
        if (!(device_scale > 0.0F)) {
            throw std::invalid_argument("SkiaPresenter received an invalid device scale");
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
        canvas.clear(color(theme.background));
        canvas.scale(device_scale, device_scale);

        SkPaint fill;
        fill.setAntiAlias(false);
        for (std::size_t region_index = 0; region_index < scene.regions.size(); ++region_index) {
            const ui::Region& region = scene.regions[region_index];
            const SkRect bounds = pixel_rect(region.rect, cell_width, cell_height);
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
            canvas.drawRect(bounds, fill);

            canvas.save();
            canvas.clipRect(bounds);
            for (std::size_t primitive_index = 0; primitive_index < region.prims.size();
                 ++primitive_index) {
                const ui::Prim& prim = region.prims[primitive_index];
                const int absolute_column = region.rect.col + prim.col;
                const int absolute_row = region.rect.row + prim.row;
                const SkScalar x = static_cast<SkScalar>(absolute_column * cell_width);
                const SkScalar top = static_cast<SkScalar>(absolute_row * cell_height);
                const SkRect cell_bounds = SkRect::MakeXYWH(
                    x, top,
                    static_cast<SkScalar>(std::max(1, ui::display_width(prim.text)) * cell_width),
                    static_cast<SkScalar>(cell_height));

                if (prim.selected) {
                    fill.setColor(color(theme.selection_background));
                    canvas.drawRect(SkRect::MakeXYWH(x, top,
                                                     static_cast<SkScalar>(
                                                         ui::display_width(prim.text) * cell_width),
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
                        draw_bounds = SkRect::MakeXYWH(x + 1.0F, top + 1.0F,
                                                       static_cast<SkScalar>(cell_width - 2), 2.0F);
                    } else {
                        draw_bounds = SkRect::MakeXYWH(x + 1.0F, top + 1.0F, 3.0F,
                                                       static_cast<SkScalar>(cell_height - 2));
                    }
                } else if ((blob = shape_text(prim.text, x, top))) {
                    shape_bounds = blob->bounds();
                    draw_bounds = shape_bounds;
                }

                std::optional<PixelProbe> probe;
                if (diagnostics && draw_bounds) {
                    SkRect probe_bounds = cell_bounds;
                    probe_bounds.join(*draw_bounds);
                    probe = capture_probe(probe_bounds, pixels, row_bytes, pixel_width,
                                          pixel_height, device_scale);
                }
                if (prim.kind != ui::PrimKind::Text && draw_bounds) {
                    canvas.drawRect(*draw_bounds, paint);
                } else if (blob) {
                    canvas.drawTextBlob(blob, 0.0F, 0.0F, paint);
                }

                if (diagnostics) {
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
                        primitive.paint_bounds =
                            changed_pixel_bounds(*probe, pixels, row_bytes, device_scale);
                    }
                    if (primitive.paint_bounds) {
                        const SkRect raster_bounds = SkRect::MakeXYWH(
                            primitive.paint_bounds->x, primitive.paint_bounds->y,
                            primitive.paint_bounds->width, primitive.paint_bounds->height);
                        primitive.row_overflow = extends_vertically(raster_bounds, cell_bounds);
                        primitive.column_overflow =
                            extends_horizontally(raster_bounds, cell_bounds);
                    }
                    diagnostics->primitives.push_back(std::move(primitive));
                }
            }
            canvas.restore();
        }

        if (scene.cursor_visible) {
            fill.setColor(color(theme.cursor));
            const SkScalar cursor_x =
                static_cast<SkScalar>(std::max(0, scene.cursor_col - 1) * cell_width);
            const SkScalar cursor_y =
                static_cast<SkScalar>(std::max(0, scene.cursor_row - 1) * cell_height);
            canvas.drawRect(
                SkRect::MakeXYWH(cursor_x, cursor_y, 2.0F, static_cast<SkScalar>(cell_height)),
                fill);
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

void SkiaPresenter::render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                           std::size_t row_bytes, float device_scale,
                           SkiaRenderDiagnostics* diagnostics) {
    impl_->render(scene, pixel_width, pixel_height, pixels, row_bytes, device_scale, diagnostics);
}

} // namespace cind::gui
