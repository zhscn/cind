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

    void draw_text(SkCanvas& canvas, std::string_view text, SkScalar x, SkScalar y,
                   const SkPaint& paint) {
        if (text.empty()) {
            return;
        }
        SkTextBlobBuilderRunHandler handler(text.data(), SkPoint::Make(x, y));
        shaper->shape(text.data(), text.size(), font, true, SK_ScalarMax, &handler);
        if (sk_sp<SkTextBlob> blob = handler.makeBlob()) {
            canvas.drawTextBlob(blob, 0.0F, 0.0F, paint);
        }
    }

    void render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                std::size_t row_bytes, float device_scale) {
        if (!(device_scale > 0.0F)) {
            throw std::invalid_argument("SkiaPresenter received an invalid device scale");
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
        for (const ui::Region& region : scene.regions) {
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
            for (const ui::Prim& prim : region.prims) {
                const int absolute_column = region.rect.col + prim.col;
                const int absolute_row = region.rect.row + prim.row;
                const SkScalar x = static_cast<SkScalar>(absolute_column * cell_width);
                const SkScalar top = static_cast<SkScalar>(absolute_row * cell_height);

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
                if (prim.kind != ui::PrimKind::Text) {
                    if (prim.kind == ui::PrimKind::ChangeDeletion) {
                        canvas.drawRect(SkRect::MakeXYWH(x + 1.0F, top + 1.0F,
                                                         static_cast<SkScalar>(cell_width - 2),
                                                         2.0F),
                                        paint);
                    } else {
                        canvas.drawRect(SkRect::MakeXYWH(x + 1.0F, top + 1.0F, 3.0F,
                                                         static_cast<SkScalar>(cell_height - 2)),
                                        paint);
                    }
                    continue;
                }
                draw_text(canvas, prim.text, x, top, paint);
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
                           std::size_t row_bytes, float device_scale) {
    impl_->render(scene, pixel_width, pixel_height, pixels, row_bytes, device_scale);
}

} // namespace cind::gui
