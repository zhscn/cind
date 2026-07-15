#pragma once

#include "gui/editor_model.hpp"
#include "ui/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cind::gui {

struct ThemeInspection {
    std::uint32_t background = 0;
    std::uint32_t gutter_background = 0;
    std::uint32_t status_background = 0;
    std::uint32_t echo_background = 0;
    std::uint32_t selection_background = 0;
    std::uint32_t cursor = 0;
    std::uint32_t sign_added = 0;
    std::uint32_t sign_modified = 0;
    std::uint32_t sign_deleted = 0;
};

struct RenderStateSnapshot {
    std::string video_driver;
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
    ThemeInspection theme;
    std::uint64_t pixel_hash = 0;
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
    static constexpr int schema_version = 1;

    std::uint64_t frame_id = 0;
    std::uint64_t cause_event_sequence = 0;
    EditorStateSnapshot editor;
    ui::Scene scene;
    RenderStateSnapshot render;
    std::vector<InputEventSnapshot> recent_events;
    std::vector<std::string> violations;
};

class InspectionHub {
public:
    std::uint64_t record_event(InputEventSnapshot event);
    void publish(EditorStateSnapshot editor, ui::Scene scene, RenderStateSnapshot render,
                 std::uint64_t cause_event_sequence);

    std::shared_ptr<const FrameInspection> latest() const;
    std::vector<InputEventSnapshot> recent_events() const;

private:
    static constexpr std::size_t max_events = 256;

    mutable std::mutex mutex_;
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
