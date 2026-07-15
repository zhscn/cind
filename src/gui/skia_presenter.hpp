#pragma once

#include "ui/scene.hpp"
#include "ui/scene_damage.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

struct SkiaLogicalRect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct SkiaLogicalPoint {
    float x = 0.0F;
    float y = 0.0F;
};

// Transient presentation state layered over a composed Scene. A scroll frame
// paints the source and target grid layers at independent pixel offsets while
// keeping bottom-anchored regions fixed. cursor_position replaces the Scene's
// cell-aligned cursor for the current frame.
struct SkiaAnimationFrame {
    const ui::Scene* scroll_source = nullptr;
    float source_grid_offset_y = 0.0F;
    float target_grid_offset_y = 0.0F;
    std::optional<SkiaLogicalPoint> cursor_position;
};

struct SkiaPrimitiveRenderDiagnostics {
    std::size_t region_index = 0;
    std::size_t primitive_index = 0;
    SkiaLogicalRect cell_bounds;
    std::optional<SkiaLogicalRect> shape_bounds;
    std::optional<SkiaLogicalRect> paint_bounds;
    bool draw_bounds_cross_region_clip = false;
    bool row_overflow = false;
    bool column_overflow = false;
};

struct SkiaRenderDiagnostics {
    float ascent = 0.0F;
    float descent = 0.0F;
    float leading = 0.0F;
    float baseline_from_row_top = 0.0F;
    std::vector<SkiaPrimitiveRenderDiagnostics> primitives;
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
    const std::string& font_family() const;
    float font_size() const;
    const SkiaTheme& theme() const;
    std::vector<SkiaLogicalRect> damage_rects(const ui::Scene& scene, const ui::SceneDamage& damage,
                                              float viewport_width, float viewport_height) const;

    void render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                std::size_t row_bytes, float device_scale = 1.0F,
                SkiaRenderDiagnostics* diagnostics = nullptr);
    void render_damage(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                       std::size_t row_bytes, std::span<const SkiaLogicalRect> damage,
                       float device_scale = 1.0F);
    void render_animated(const ui::Scene& scene, const SkiaAnimationFrame& animation,
                         int pixel_width, int pixel_height, void* pixels, std::size_t row_bytes,
                         float device_scale = 1.0F, SkiaRenderDiagnostics* diagnostics = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind::gui
