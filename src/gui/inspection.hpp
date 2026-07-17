#pragma once

#include "gui/editor_state.hpp"
#include "ui/scene.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind::gui {

struct ThemeInspection {
    std::uint32_t canvas = 0;
    std::uint32_t highlight = 0;
    std::uint32_t band = 0;
    std::uint32_t selection = 0;
    std::uint32_t divider = 0;
    std::uint32_t text = 0;
    std::uint32_t strong = 0;
    std::uint32_t faded = 0;
    std::uint32_t faint = 0;
    std::uint32_t salient = 0;
    std::uint32_t popout = 0;
    std::uint32_t critical = 0;
    std::uint32_t cursor = 0;
    std::uint32_t sign_added = 0;
    std::uint32_t sign_modified = 0;
    std::uint32_t sign_deleted = 0;
};

struct LogicalPixelPointSnapshot {
    float x = 0.0F;
    float y = 0.0F;
};

struct LogicalPixelRectSnapshot {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct OutputPixelRectSnapshot {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RenderDamageRectSnapshot {
    LogicalPixelRectSnapshot logical;
    OutputPixelRectSnapshot output;
};

struct RenderDamageSnapshot {
    bool full_repaint = false;
    bool grid_transform_changed = false;
    float grid_translation_rows = 0.0F;
    std::size_t damaged_cells = 0;
    std::uint64_t damaged_output_pixels = 0;
    double output_fraction = 0.0;
    bool full_reference_match = true;
    std::vector<RenderDamageRectSnapshot> rects;
};

struct RenderScrollLayerSnapshot {
    float scroll_top = 0.0F;
    float grid_offset_y = 0.0F;
    float clip_top = 0.0F;
    float clip_bottom = 0.0F;
};

struct RenderAnimationSnapshot {
    bool active = false;
    bool scroll = false;
    bool cursor = false;
    bool cursor_constrained = false;
    std::string cursor_owner = "none";
    float scroll_progress = 1.0F;
    float cursor_progress = 1.0F;
    float scroll_velocity = 0.0F;
    float visual_scroll_top = 0.0F;
    float target_scroll_top = 0.0F;
    std::vector<RenderScrollLayerSnapshot> layers;
    std::optional<float> active_line_y;
    std::optional<LogicalPixelRectSnapshot> cursor_rect;
};

struct RenderTimingSnapshot {
    double layout_us = 0.0;
    double compose_us = 0.0;
    double render_state_us = 0.0;
    double inspect_us = 0.0;
    double frame_build_us = 0.0;
    double raster_us = 0.0;
    double reference_us = 0.0;
    double upload_us = 0.0;
    double present_us = 0.0;
    double total_us = 0.0;
    std::uint64_t uploaded_bytes = 0;
    std::size_t upload_rects = 0;
    bool texture_scroll_reused = false;
    std::uint64_t texture_copy_pixels = 0;
    std::uint64_t shape_cache_hits = 0;
    std::uint64_t shape_cache_misses = 0;
    std::uint64_t shape_cache_evictions = 0;
    std::size_t shape_cache_entries = 0;
};

struct FontMetricsSnapshot {
    float ascent = 0.0F;
    float descent = 0.0F;
    float leading = 0.0F;
    float baseline_from_row_top = 0.0F;
};

struct PrimitiveRenderSnapshot {
    std::size_t region_index = 0;
    std::size_t primitive_index = 0;
    std::string id;
    std::string region;
    std::string kind;
    LogicalPixelRectSnapshot layout_bounds;
    std::optional<LogicalPixelRectSnapshot> shape_bounds;
    std::optional<LogicalPixelRectSnapshot> paint_bounds;
    bool draw_bounds_cross_region_clip = false;
    bool row_overflow = false;
    bool column_overflow = false;
};

struct TextLayoutSnapshot {
    std::string role;
    std::size_t byte_count = 0;
    float advance = 0.0F;
    LogicalPixelPointSnapshot origin;
    std::optional<LogicalPixelRectSnapshot> shape_bounds;
};

struct PopupLayoutSnapshot {
    LogicalPixelRectSnapshot panel_bounds;
    LogicalPixelRectSnapshot header_bounds;
    float horizontal_scroll = 0.0F;
    std::size_t input_bytes = 0;
    std::size_t input_cursor = 0;
    float cursor_advance = 0.0F;
    float unclamped_cursor_x = 0.0F;
    bool cursor_clamped = false;
    std::optional<LogicalPixelRectSnapshot> cursor_rect;
    std::vector<TextLayoutSnapshot> header_text;
};

struct EchoLayoutSnapshot {
    LogicalPixelRectSnapshot bounds;
    float horizontal_scroll = 0.0F;
    std::size_t text_bytes = 0;
    std::optional<std::size_t> cursor_byte;
    float cursor_advance = 0.0F;
    float unclamped_cursor_x = 0.0F;
    bool cursor_clamped = false;
    std::optional<LogicalPixelRectSnapshot> cursor_rect;
    TextLayoutSnapshot text;
};

struct DocumentLineLayoutSnapshot {
    int row = 0;
    int end_column = 0;
    float origin_x = 0.0F;
    float advance = 0.0F;
    std::size_t run_count = 0;
};

struct DocumentLayoutSnapshot {
    LogicalPixelRectSnapshot bounds;
    std::optional<int> cursor_row;
    std::optional<int> cursor_column;
    float cursor_advance = 0.0F;
    float grid_cursor_x = 0.0F;
    std::optional<LogicalPixelRectSnapshot> cursor_rect;
    std::vector<DocumentLineLayoutSnapshot> lines;
};

struct RenderStateSnapshot {
    std::string video_driver;
    std::string render_driver;
    int window_width = 0;
    int window_height = 0;
    int output_width = 0;
    int output_height = 0;
    float display_scale = 1.0F;
    int cell_width = 0;
    int cell_height = 0;
    int rows = 0;
    int columns = 0;
    std::string texture_format;
    std::string font_family;
    float font_size = 0.0F;
    FontMetricsSnapshot font_metrics;
    ThemeInspection theme;
    std::uint64_t pixel_hash = 0;
    RenderAnimationSnapshot animation;
    RenderDamageSnapshot damage;
    RenderTimingSnapshot timings;
    std::optional<DocumentLayoutSnapshot> document_layout;
    std::optional<PopupLayoutSnapshot> popup_layout;
    std::optional<EchoLayoutSnapshot> echo_layout;
    std::vector<PrimitiveRenderSnapshot> primitives;
};

struct InputEventSnapshot {
    std::uint64_t sequence = 0;
    std::string type;
    std::string detail;
    bool handled = false;
    bool repaint = false;
    RevisionId revision_before = 0;
    RevisionId revision_after = 0;
};

struct FrameInspection {
    static constexpr int schema_version = 43;

    std::uint64_t frame_id = 0;
    std::uint64_t cause_event_sequence = 0;
    EditorStateSnapshot editor;
    ui::Scene scene;
    RenderStateSnapshot render;
    std::vector<InputEventSnapshot> recent_events;
    std::vector<std::string> violations;
};

struct InspectionState {
    std::shared_ptr<const FrameInspection> frame;
    std::vector<InputEventSnapshot> events;
    std::uint64_t oldest_event_sequence = 1;
    std::uint64_t last_event_sequence = 0;
};

class InspectionHub {
public:
    std::uint64_t record_event(InputEventSnapshot event);
    void publish(EditorStateSnapshot editor, ui::Scene scene, RenderStateSnapshot render,
                 std::uint64_t cause_event_sequence);

    std::shared_ptr<const FrameInspection> latest() const;
    std::vector<InputEventSnapshot> recent_events() const;
    InspectionState snapshot() const;
    InspectionState wait_for_frame(std::uint64_t after_frame,
                                   std::chrono::milliseconds timeout) const;
    InspectionState wait_for_events(std::uint64_t after_event,
                                    std::chrono::milliseconds timeout) const;

private:
    static constexpr std::size_t max_events = 256;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::shared_ptr<const FrameInspection> latest_;
    std::deque<InputEventSnapshot> events_;
    std::uint64_t next_event_sequence_ = 1;
    std::uint64_t next_frame_id_ = 1;
};

struct InspectionResponse {
    bool ok = false;
    std::string payload;
};

std::string_view region_role_name(ui::RegionRole role);
std::string_view style_class_name(ui::StyleClass style);

std::string inspection_snapshot_json(const FrameInspection& frame);
std::string inspection_tree_text(const FrameInspection& frame);
InspectionResponse run_inspection_query(const InspectionHub& hub, std::string_view request);

} // namespace cind::gui
