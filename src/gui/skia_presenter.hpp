#pragma once

#include "ui/scene.hpp"
#include "ui/scene_damage.hpp"
#include "ui/scene_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cind::gui {

// One canvas, one surface: the editor body, gutter, and echo strip share the
// canvas ground; the modeline and floating panels are the only raised
// surfaces. Every separator is the same translucent hairline.
struct SkiaTheme {
    std::uint32_t canvas = 0xFF1B1D22;
    std::uint32_t surface = 0xFF24262C;
    std::uint32_t raised = 0xFF2F333B;
    std::uint32_t hairline = 0x14FFFFFF;
    std::uint32_t active_line = 0xFF22242A;
    std::uint32_t selection = 0xFF2C4568;
    std::uint32_t text = 0xFFD7DAE0;
    std::uint32_t strong = 0xFFF2F4F8;
    std::uint32_t muted = 0xFF8A919C;
    std::uint32_t faint = 0xFF555B66;
    std::uint32_t accent = 0xFF7AA8F5;
    std::uint32_t cursor = 0xFFE8EAEE;
    std::uint32_t shadow = 0x4D000000;
    std::uint32_t sign_added = 0xFF6F9A1E;
    std::uint32_t sign_modified = 0xFF1E93B4;
    std::uint32_t sign_deleted = 0xFFB03038;
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

// Adds the previous and current cursor bounds when the presented cursor moves.
// This bridges transient animation positions into a retained partial-render
// frame, whose scene damage only knows cell-aligned logical cursor targets.
void append_cursor_transition_damage(std::vector<SkiaLogicalRect>& damage,
                                     const std::optional<SkiaLogicalRect>& previous_cursor,
                                     const std::optional<SkiaLogicalRect>& current_cursor);

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
    SkiaLogicalRect layout_bounds;
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
    float status_bar_height() const;
    float echo_area_height() const;
    // Vertical metrics with this presenter's footer heights, for callers that
    // need the same row mapping the painter uses (mouse hits, inspection).
    ui::SceneVerticalMetrics vertical_metrics(float viewport_height) const;
    // Shows the faint revision segment at the modeline's right edge.
    void set_show_debug_status(bool show);

    // Returns the graphical caret bounds in logical pixels. A popup prompt
    // owns the caret while an interactive picker is active.
    std::optional<SkiaLogicalRect> cursor_rect(const ui::Scene& scene, float viewport_width,
                                               float viewport_height) const;
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
