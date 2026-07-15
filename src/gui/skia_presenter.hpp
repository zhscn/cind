#pragma once

#include "ui/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cind::gui {

struct SkiaTheme {
    std::uint32_t background = 0xFF1E1E1E;
    std::uint32_t gutter_background = 0xFF181818;
    std::uint32_t status_background = 0xFF3A3D41;
    std::uint32_t echo_background = 0xFF1E1E1E;
    std::uint32_t selection_background = 0xFF264F78;
    std::uint32_t cursor = 0xFFD4D4D4;
    std::uint32_t sign_added = 0xFF587C0C;
    std::uint32_t sign_modified = 0xFF0C7D9D;
    std::uint32_t sign_deleted = 0xFF94151B;
};

// Raster Skia presenter for the backend-independent cell Scene. The caller
// owns an N32-premultiplied pixel buffer and decides how to put it on screen.
// Text uses a monospace cell grid, direct UTF-8 glyph drawing, and system font
// fallback per non-ASCII code point; shaping and bidirectional layout are
// outside this presenter's contract.
class SkiaPresenter {
public:
    explicit SkiaPresenter(std::string font_family = "monospace", float font_size = 16.0F,
                           SkiaTheme theme = {});
    ~SkiaPresenter();

    SkiaPresenter(const SkiaPresenter&) = delete;
    SkiaPresenter& operator=(const SkiaPresenter&) = delete;
    SkiaPresenter(SkiaPresenter&&) noexcept;
    SkiaPresenter& operator=(SkiaPresenter&&) noexcept;

    int cell_width() const;
    int cell_height() const;

    void render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                std::size_t row_bytes, float device_scale = 1.0F);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind::gui
