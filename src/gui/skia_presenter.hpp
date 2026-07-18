#pragma once

#include "presentation/theme.hpp"
#include "ui/scene.hpp"
#include "ui/scene_damage.hpp"
#include "ui/scene_layout.hpp"
#include "ui/view_tree.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cind::gui {

// Glyph anti-aliasing strategy. Fractional HiDPI (e.g. 1.5x Wayland) makes LCD
// subpixel AA muddy because RGB stripes cannot align to a fractional grid, so
// the default renders grayscale coverage and lets slight hinting snap vertical
// stems onto the pixel grid — closer to the macOS look than the fuzzy LCD path.
enum class SkiaFontSmoothing : std::uint8_t {
    // Grayscale, true outlines positioned at subpixel offsets, no grid fit.
    // Softest; the pure macOS/CoreText look. The default — it won a blind
    // preference test against the hinted and LCD alternatives at 16px.
    Smooth,
    // Grayscale with slight autohinting so vertical stems land on whole
    // pixels. Reads a touch heavier and crisper.
    Crisp,
    // Grayscale, full grid-fit hinting, no subpixel positioning. Sharpest,
    // most ClearType-grayscale/Windows-like.
    Sharp,
    // The historical LCD subpixel path, kept for comparison.
    LcdSubpixel,
};

SkiaFontSmoothing parse_font_smoothing(std::string_view name);

using SkiaTheme = PresentationTheme;

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

struct SkiaShapeCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::size_t entries = 0;
};

struct SkiaGridTranslation {
    int output_rows = 0;
    int grid_output_bottom = 0;
};

enum class SkiaCursorOwner : std::uint8_t {
    None,
    Document,
    Popup,
    Echo,
    Other,
};

// Pixel-space state for the transient view overlay. It is derived once from
// the prepared shaped layout, then shared by animation, painting, damage, and
// inspection so caret and active-line presentation cannot drift apart.
struct SkiaViewPresentation {
    SkiaCursorOwner cursor_owner = SkiaCursorOwner::None;
    std::optional<SkiaLogicalRect> cursor_rect;
    std::optional<float> active_line_y;
};

// Interpolates one same-surface view transition. Document caret and
// active-line positions share the same progress; cross-surface focus changes
// adopt the target presentation directly.
SkiaViewPresentation interpolate_view_presentation(const SkiaViewPresentation& from,
                                                   const SkiaViewPresentation& target,
                                                   float progress);

// Adds the previous and current cursor bounds when the presented cursor moves.
// This bridges transient animation positions into a retained partial-render
// frame, whose scene damage only knows cell-aligned logical cursor targets.
void append_cursor_transition_damage(std::vector<SkiaLogicalRect>& damage,
                                     const std::optional<SkiaLogicalRect>& previous_cursor,
                                     const std::optional<SkiaLogicalRect>& current_cursor);

struct SkiaScrollLayer {
    std::shared_ptr<const ui::Scene> scene;
    float grid_offset_y = 0.0F;
    float clip_top = 0.0F;
    float clip_bottom = 0.0F;
};

// Transient presentation state layered over a composed Scene. A scroll frame
// paints the viewport snapshots adjacent to the current visual document
// position at independent pixel offsets while keeping bottom-anchored regions
// fixed. view carries the complete current-view overlay independently from
// the content snapshots.
struct SkiaAnimationFrame {
    std::vector<SkiaScrollLayer> scroll_layers;
    SkiaViewPresentation view;
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

struct SkiaTextLayoutDiagnostics {
    std::string role;
    std::size_t byte_count = 0;
    float advance = 0.0F;
    SkiaLogicalPoint origin;
    std::optional<SkiaLogicalRect> shape_bounds;
};

struct SkiaPopupLayoutDiagnostics {
    SkiaLogicalRect panel_bounds;
    SkiaLogicalRect header_bounds;
    float horizontal_scroll = 0.0F;
    std::size_t input_bytes = 0;
    std::size_t input_cursor = 0;
    float cursor_advance = 0.0F;
    float unclamped_cursor_x = 0.0F;
    bool cursor_clamped = false;
    std::optional<SkiaLogicalRect> cursor_rect;
    std::vector<SkiaTextLayoutDiagnostics> header_text;
};

struct SkiaEchoLayoutDiagnostics {
    SkiaLogicalRect bounds;
    float horizontal_scroll = 0.0F;
    std::size_t text_bytes = 0;
    std::optional<std::size_t> cursor_byte;
    float cursor_advance = 0.0F;
    float unclamped_cursor_x = 0.0F;
    bool cursor_clamped = false;
    std::optional<SkiaLogicalRect> cursor_rect;
    SkiaTextLayoutDiagnostics text;
};

struct SkiaDocumentLineLayoutDiagnostics {
    int row = 0;
    int end_column = 0;
    float origin_x = 0.0F;
    float advance = 0.0F;
    std::size_t run_count = 0;
};

struct SkiaDocumentLayoutDiagnostics {
    SkiaLogicalRect bounds;
    std::optional<int> cursor_row;
    std::optional<int> cursor_column;
    float cursor_advance = 0.0F;
    float grid_cursor_x = 0.0F;
    std::optional<SkiaLogicalRect> cursor_rect;
    std::vector<SkiaDocumentLineLayoutDiagnostics> lines;
};

struct SkiaRenderDiagnostics {
    float ascent = 0.0F;
    float descent = 0.0F;
    float leading = 0.0F;
    float baseline_from_row_top = 0.0F;
    std::optional<SkiaDocumentLayoutDiagnostics> document_layout;
    std::optional<SkiaPopupLayoutDiagnostics> popup_layout;
    std::optional<SkiaEchoLayoutDiagnostics> echo_layout;
    std::vector<SkiaPrimitiveRenderDiagnostics> primitives;
};

enum class SkiaHitTestScope : std::uint8_t {
    All,
    Grid,
    Fixed,
};

// Immutable, frame-scoped pixel layout prepared from one stable Scene and
// logical viewport. The source Scene must outlive the layout and remain
// unchanged. Painting, cursor geometry, damage conversion, IME placement, and
// diagnostics consume this object so shaped text and chrome geometry have one
// owner. A layout is consumed by the presenter that prepared it.
class SkiaFrameLayout {
public:
    ~SkiaFrameLayout();
    SkiaFrameLayout(SkiaFrameLayout&&) noexcept;
    SkiaFrameLayout& operator=(SkiaFrameLayout&&) noexcept;

    SkiaFrameLayout(const SkiaFrameLayout&) = delete;
    SkiaFrameLayout& operator=(const SkiaFrameLayout&) = delete;

private:
    struct Storage;
    explicit SkiaFrameLayout(std::unique_ptr<Storage> storage);

    std::unique_ptr<Storage> storage_;
    friend class SkiaPresenter;
};

struct SkiaPreparedScrollLayer {
    std::shared_ptr<const ui::Scene> scene;
    std::shared_ptr<const SkiaFrameLayout> layout;
    float grid_offset_y = 0.0F;
    float clip_top = 0.0F;
    float clip_bottom = 0.0F;
};

// Frame-scoped animation input whose scroll-layer Scenes have already been
// shaped and laid out. Painting and hit-testing consume these exact layouts.
struct SkiaPreparedAnimationFrame {
    std::vector<SkiaPreparedScrollLayer> scroll_layers;
    SkiaViewPresentation view;
};

// Raster Skia presenter for the backend-independent cell Scene. The caller
// owns an N32-premultiplied pixel buffer and decides how to put it on screen.
// The Scene retains cell semantics for TUI parity; the prepared frame layout
// converts document lines, carets, hit tests, and GUI chrome to one Skia-shaped
// logical-pixel coordinate space with system font fallback.
class SkiaPresenter {
public:
    explicit SkiaPresenter(std::string font_family, float font_size, SkiaTheme theme,
                           SkiaFontSmoothing smoothing = SkiaFontSmoothing::Smooth);
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
    SkiaShapeCacheStats shape_cache_stats() const;
    // Vertical metrics with this presenter's footer heights, for callers that
    // need the same row mapping the painter uses (mouse hits, inspection).
    ui::SceneVerticalMetrics vertical_metrics(float viewport_height) const;
    SkiaFrameLayout prepare_layout(const ui::Scene& scene, float viewport_width,
                                   float viewport_height) const;
    SkiaPreparedAnimationFrame prepare_animation_frame(const SkiaAnimationFrame& animation,
                                                       float viewport_width,
                                                       float viewport_height) const;
    // Includes modeline segments marked as debug-only.
    void set_show_debug_status(bool show);

    // Returns the graphical caret bounds in logical pixels. A popup prompt
    // owns the caret while an interactive picker is active.
    SkiaViewPresentation view_presentation(const SkiaFrameLayout& layout) const;
    std::optional<SkiaLogicalRect> cursor_rect(const ui::Scene& scene, float viewport_width,
                                               float viewport_height) const;
    std::optional<SkiaLogicalRect> cursor_rect(const SkiaFrameLayout& layout) const;
    // Maps a logical pixel point through the prepared text layout. Document
    // rows use their shaped advances; cell-oriented regions retain grid hits.
    ui::CellPoint hit_test(const SkiaFrameLayout& layout, SkiaLogicalPoint point) const;
    std::optional<ui::ViewHit> hit_test_view(const SkiaFrameLayout& layout, SkiaLogicalPoint point,
                                             SkiaHitTestScope scope = SkiaHitTestScope::All) const;
    std::vector<SkiaLogicalRect> damage_rects(const ui::Scene& scene, const ui::SceneDamage& damage,
                                              float viewport_width, float viewport_height) const;
    std::vector<SkiaLogicalRect> damage_rects(const SkiaFrameLayout& layout,
                                              const ui::SceneDamage& damage) const;

    void render(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                std::size_t row_bytes, float device_scale = 1.0F,
                SkiaRenderDiagnostics* diagnostics = nullptr);
    void render(const SkiaFrameLayout& layout, int pixel_width, int pixel_height, void* pixels,
                std::size_t row_bytes, float device_scale = 1.0F,
                SkiaRenderDiagnostics* diagnostics = nullptr);
    void render_damage(const ui::Scene& scene, int pixel_width, int pixel_height, void* pixels,
                       std::size_t row_bytes, std::span<const SkiaLogicalRect> damage,
                       float device_scale = 1.0F);
    void render_damage(const SkiaFrameLayout& layout, int pixel_width, int pixel_height,
                       void* pixels, std::size_t row_bytes, std::span<const SkiaLogicalRect> damage,
                       float device_scale = 1.0F);
    // Reuses the retained raster when a fractional scroll translates otherwise unchanged grid
    // content by a whole number of output pixels. Returns no translation when the frame needs
    // normal damage rendering instead.
    std::optional<SkiaGridTranslation>
    render_grid_translation_damage(const SkiaFrameLayout& layout, const ui::SceneDamage& damage,
                                   int pixel_width, int pixel_height, void* pixels,
                                   std::size_t row_bytes, float device_scale = 1.0F);
    void render_animated(const ui::Scene& scene, const SkiaAnimationFrame& animation,
                         int pixel_width, int pixel_height, void* pixels, std::size_t row_bytes,
                         float device_scale = 1.0F, SkiaRenderDiagnostics* diagnostics = nullptr);
    void render_animated(const SkiaFrameLayout& layout, const SkiaAnimationFrame& animation,
                         int pixel_width, int pixel_height, void* pixels, std::size_t row_bytes,
                         float device_scale = 1.0F, SkiaRenderDiagnostics* diagnostics = nullptr);
    void render_animated(const SkiaFrameLayout& layout, const SkiaPreparedAnimationFrame& animation,
                         int pixel_width, int pixel_height, void* pixels, std::size_t row_bytes,
                         float device_scale = 1.0F, SkiaRenderDiagnostics* diagnostics = nullptr);

private:
    struct Impl;
    const SkiaFrameLayout::Storage& checked_layout(const SkiaFrameLayout& layout) const;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind::gui
