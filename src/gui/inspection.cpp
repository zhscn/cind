#include "gui/inspection.hpp"

#include "ui/char_width.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace cind::gui {

namespace {

void append_json_string(std::string& output, std::string_view value) {
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                output += std::format("\\u{:04x}", byte);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_bool(std::string& output, bool value) {
    output += value ? "true" : "false";
}

void append_rect(std::string& output, const ui::Rect& rect) {
    output += std::format("{{\"row\":{},\"col\":{},\"rows\":{},\"cols\":{}}}", rect.row, rect.col,
                          rect.rows, rect.cols);
}

void append_line_signs(std::string& output, const ui::LineSigns& signs) {
    output += std::format("{{\"first\":{},\"modified\":{},\"added\":{},\"deleted\":", signs.first,
                          signs.modified, signs.added);
    append_bool(output, signs.deleted);
    output += std::format(",\"boundary\":{}}}", signs.boundary);
}

void append_editor(std::string& output, const EditorStateSnapshot& editor) {
    output += "{\"path\":";
    append_json_string(output, editor.path);
    output += std::format(",\"revision\":{},\"document_bytes\":{},\"line_count\":{},\"dirty\":",
                          editor.revision, editor.document_bytes, editor.line_count);
    append_bool(output, editor.dirty);
    output += std::format(
        ",\"caret\":{{\"byte\":{},\"line\":{},\"byte_column\":{},\"display_column\":{}}}",
        editor.caret.value, editor.caret_position.line, editor.caret_position.byte_column,
        editor.caret_display_column);
    output += std::format(",\"viewport\":{{\"top_line\":{},\"left_column\":{}}},\"line_signs\":",
                          editor.viewport.top_line, editor.viewport.left_column);
    append_line_signs(output, editor.line_signs);
    output += std::format(",\"tab_width\":{},\"style_origin\":", editor.tab_width);
    append_json_string(output, editor.style_origin);
    output += ",\"message\":";
    append_json_string(output, editor.message);
    output += ",\"preedit\":";
    append_json_string(output, editor.preedit);
    output += ",\"last_key\":";
    append_json_string(output, editor.last_key);
    output += ",\"quit_armed\":";
    append_bool(output, editor.quit_armed);
    output += ",\"quit\":";
    append_bool(output, editor.quit);
    output.push_back('}');
}

void append_prim(std::string& output, const ui::Region& region, std::size_t index) {
    const ui::Prim& prim = region.prims[index];
    output += "{\"id\":";
    append_json_string(output,
                       std::format("region:{}/prim:{}", region_role_name(region.role), index));
    output += std::format(",\"row\":{},\"col\":{},\"text\":", prim.row, prim.col);
    append_json_string(output, prim.text);
    output += ",\"style\":";
    append_json_string(output, style_class_name(prim.style));
    output += ",\"selected\":";
    append_bool(output, prim.selected);
    output.push_back('}');
}

void append_region(std::string& output, const ui::Region& region) {
    output += "{\"id\":";
    append_json_string(output, std::format("region:{}", region_role_name(region.role)));
    output += ",\"role\":";
    append_json_string(output, region_role_name(region.role));
    output += ",\"rect\":";
    append_rect(output, region.rect);
    output += ",\"prims\":[";
    for (std::size_t index = 0; index < region.prims.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_prim(output, region, index);
    }
    output += "]}";
}

void append_scene(std::string& output, const ui::Scene& scene) {
    output +=
        std::format("{{\"rows\":{},\"cols\":{},\"cursor\":{{\"row\":{},\"col\":{}}},\"regions\":[",
                    scene.rows, scene.cols, scene.cursor_row, scene.cursor_col);
    for (std::size_t index = 0; index < scene.regions.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_region(output, scene.regions[index]);
    }
    output += "]}";
}

void append_color(std::string& output, std::uint32_t argb) {
    append_json_string(output, std::format("#{:08X}", argb));
}

void append_render(std::string& output, const RenderStateSnapshot& render) {
    output += "{\"video_driver\":";
    append_json_string(output, render.video_driver);
    output += std::format(
        ",\"window\":{{\"width\":{},\"height\":{}}},\"output\":{{\"width\":{},\"height\":{}}},"
        "\"display_scale\":{},\"cell\":{{\"width\":{},\"height\":{}}},\"grid\":{{\"rows\":{},"
        "\"columns\":{}}},\"texture_format\":",
        render.window_width, render.window_height, render.output_width, render.output_height,
        render.display_scale, render.cell_width, render.cell_height, render.rows, render.columns);
    append_json_string(output, render.texture_format);
    output += ",\"font\":{\"family\":";
    append_json_string(output, render.font_family);
    output += std::format(",\"size\":{}}},\"theme\":{{\"background\":", render.font_size);
    append_color(output, render.theme.background);
    output += ",\"gutter_background\":";
    append_color(output, render.theme.gutter_background);
    output += ",\"status_background\":";
    append_color(output, render.theme.status_background);
    output += ",\"echo_background\":";
    append_color(output, render.theme.echo_background);
    output += ",\"selection_background\":";
    append_color(output, render.theme.selection_background);
    output += ",\"cursor\":";
    append_color(output, render.theme.cursor);
    output += ",\"sign_added\":";
    append_color(output, render.theme.sign_added);
    output += ",\"sign_modified\":";
    append_color(output, render.theme.sign_modified);
    output += ",\"sign_deleted\":";
    append_color(output, render.theme.sign_deleted);
    output += std::format("}},\"pixel_hash\":\"0x{:016X}\"}}", render.pixel_hash);
}

void append_event(std::string& output, const InputEventSnapshot& event) {
    output += std::format("{{\"sequence\":{},\"type\":", event.sequence);
    append_json_string(output, event.type);
    output += ",\"detail\":";
    append_json_string(output, event.detail);
    output += ",\"handled\":";
    append_bool(output, event.handled);
    output += ",\"repaint\":";
    append_bool(output, event.repaint);
    output += std::format(",\"revision_before\":{},\"revision_after\":{}}}", event.revision_before,
                          event.revision_after);
}

void append_events(std::string& output, const std::vector<InputEventSnapshot>& events,
                   std::uint64_t after_sequence = 0) {
    output.push_back('[');
    bool first = true;
    for (const InputEventSnapshot& event : events) {
        if (event.sequence <= after_sequence) {
            continue;
        }
        if (!first) {
            output.push_back(',');
        }
        append_event(output, event);
        first = false;
    }
    output.push_back(']');
}

void append_strings(std::string& output, const std::vector<std::string>& values) {
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_json_string(output, values[index]);
    }
    output.push_back(']');
}

std::vector<std::string> validate_frame(const FrameInspection& frame) {
    std::vector<std::string> violations;
    if (frame.editor.line_count == 0) {
        violations.emplace_back("editor line_count must be positive");
    }
    if (frame.editor.caret.value > frame.editor.document_bytes) {
        violations.emplace_back("editor caret is past the document end");
    }
    if (frame.editor.line_count > 0 && frame.editor.viewport.top_line >= frame.editor.line_count) {
        violations.emplace_back("editor viewport starts past the document end");
    }
    if (frame.scene.rows <= 0 || frame.scene.cols <= 0) {
        violations.emplace_back("scene geometry must be positive");
    }
    if (frame.scene.cursor_row < 1 || frame.scene.cursor_row > frame.scene.rows ||
        frame.scene.cursor_col < 1 || frame.scene.cursor_col > frame.scene.cols) {
        violations.emplace_back("scene cursor is outside the scene");
    }
    for (const ui::Region& region : frame.scene.regions) {
        if (region.rect.row < 0 || region.rect.col < 0 || region.rect.rows < 0 ||
            region.rect.cols < 0 || region.rect.row + region.rect.rows > frame.scene.rows ||
            region.rect.col + region.rect.cols > frame.scene.cols) {
            violations.push_back(
                std::format("region:{} is outside the scene", region_role_name(region.role)));
        }
        for (std::size_t index = 0; index < region.prims.size(); ++index) {
            const ui::Prim& prim = region.prims[index];
            if (prim.row < 0 || prim.row >= region.rect.rows || prim.col < 0 ||
                prim.col >= region.rect.cols) {
                violations.push_back(std::format("region:{}/prim:{} starts outside its region",
                                                 region_role_name(region.role), index));
            }
        }
    }
    if (frame.render.display_scale <= 0.0F) {
        violations.emplace_back("render display scale must be positive");
    }
    if (frame.render.window_width <= 0 || frame.render.window_height <= 0 ||
        frame.render.output_width <= 0 || frame.render.output_height <= 0) {
        violations.emplace_back("render window and output geometry must be positive");
    }
    if (frame.render.cell_width <= 0 || frame.render.cell_height <= 0) {
        violations.emplace_back("render cell geometry must be positive");
    }
    if (frame.render.rows != frame.scene.rows || frame.render.columns != frame.scene.cols) {
        violations.emplace_back("render grid does not match scene geometry");
    }
    return violations;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::string printable(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char byte : value) {
        switch (byte) {
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output.push_back(byte);
            break;
        }
    }
    return output;
}

const ui::Region* find_region(const ui::Scene& scene, std::string_view name) {
    for (const ui::Region& region : scene.regions) {
        if (region_role_name(region.role) == name) {
            return &region;
        }
    }
    return nullptr;
}

InspectionResponse get_query(const FrameInspection& frame, std::string_view path) {
    path = trim(path);
    if (path == "frame.id") {
        return {true, std::to_string(frame.frame_id)};
    }
    if (path == "frame.cause_event_sequence") {
        return {true, std::to_string(frame.cause_event_sequence)};
    }
    if (path == "event.last_sequence") {
        const std::uint64_t sequence =
            frame.recent_events.empty() ? 0 : frame.recent_events.back().sequence;
        return {true, std::to_string(sequence)};
    }
    std::string output;
    if (path == "frame.violations") {
        append_strings(output, frame.violations);
    } else if (path == "editor") {
        append_editor(output, frame.editor);
    } else if (path == "editor.caret") {
        output =
            std::format("{{\"byte\":{},\"line\":{},\"byte_column\":{},\"display_column\":{}}}",
                        frame.editor.caret.value, frame.editor.caret_position.line,
                        frame.editor.caret_position.byte_column, frame.editor.caret_display_column);
    } else if (path == "editor.viewport") {
        output = std::format("{{\"top_line\":{},\"left_column\":{}}}",
                             frame.editor.viewport.top_line, frame.editor.viewport.left_column);
    } else if (path == "editor.line_signs") {
        append_line_signs(output, frame.editor.line_signs);
    } else if (path == "scene") {
        append_scene(output, frame.scene);
    } else if (path == "scene.cursor") {
        output = std::format("{{\"row\":{},\"col\":{}}}", frame.scene.cursor_row,
                             frame.scene.cursor_col);
    } else if (path == "render") {
        append_render(output, frame.render);
    } else if (path.starts_with("scene.region.")) {
        const std::string_view name = path.substr(std::string_view("scene.region.").size());
        const ui::Region* region = find_region(frame.scene, name);
        if (!region) {
            return {false, std::format("unknown scene region '{}'", name)};
        }
        append_region(output, *region);
    } else {
        return {false, std::format("unknown inspection path '{}'", path)};
    }
    return {true, std::move(output)};
}

InspectionResponse pick_query(const FrameInspection& frame, std::string_view arguments) {
    std::istringstream input{std::string(arguments)};
    float window_x = 0.0F;
    float window_y = 0.0F;
    if (!(input >> window_x >> window_y)) {
        return {false, "usage: pick <window-x> <window-y>"};
    }
    if (frame.render.window_width <= 0 || frame.render.window_height <= 0 ||
        frame.scene.cols <= 0 || frame.scene.rows <= 0) {
        return {false, "frame has no usable window geometry"};
    }
    if (window_x < 0.0F || window_y < 0.0F ||
        window_x >= static_cast<float>(frame.render.window_width) ||
        window_y >= static_cast<float>(frame.render.window_height)) {
        return {false, "point is outside the window"};
    }

    const int cell_col =
        std::clamp(static_cast<int>(window_x / static_cast<float>(frame.render.window_width) *
                                    static_cast<float>(frame.scene.cols)),
                   0, frame.scene.cols - 1);
    const int cell_row =
        std::clamp(static_cast<int>(window_y / static_cast<float>(frame.render.window_height) *
                                    static_cast<float>(frame.scene.rows)),
                   0, frame.scene.rows - 1);

    const ui::Region* hit_region = nullptr;
    for (const ui::Region& region : frame.scene.regions) {
        if (cell_row >= region.rect.row && cell_row < region.rect.row + region.rect.rows &&
            cell_col >= region.rect.col && cell_col < region.rect.col + region.rect.cols) {
            hit_region = &region;
            break;
        }
    }

    std::string output =
        std::format("{{\"window\":{{\"x\":{},\"y\":{}}},\"cell\":{{\"row\":{},\"col\":{}}}",
                    window_x, window_y, cell_row, cell_col);
    if (!hit_region) {
        output += ",\"region\":null,\"prim\":null}";
        return {true, std::move(output)};
    }

    output += ",\"region\":";
    append_json_string(output, std::format("region:{}", region_role_name(hit_region->role)));
    const int local_row = cell_row - hit_region->rect.row;
    const int local_col = cell_col - hit_region->rect.col;
    output += std::format(",\"local_cell\":{{\"row\":{},\"col\":{}}}", local_row, local_col);
    if (hit_region->role == ui::RegionRole::TextArea ||
        hit_region->role == ui::RegionRole::LineNumbers ||
        hit_region->role == ui::RegionRole::ChangeSigns) {
        output += std::format(",\"document_line\":{}",
                              frame.editor.viewport.top_line +
                                  static_cast<std::uint32_t>(std::max(0, local_row)));
    }

    const ui::Prim* hit_prim = nullptr;
    std::size_t hit_index = 0;
    for (std::size_t index = 0; index < hit_region->prims.size(); ++index) {
        const ui::Prim& prim = hit_region->prims[index];
        const int width = std::max(1, ui::display_width(prim.text));
        if (prim.row == local_row && local_col >= prim.col && local_col < prim.col + width) {
            hit_prim = &prim;
            hit_index = index;
        }
    }
    output += ",\"prim\":";
    if (hit_prim) {
        append_prim(output, *hit_region, hit_index);
    } else {
        output += "null";
    }
    output.push_back('}');
    return {true, std::move(output)};
}

} // namespace

std::uint64_t InspectionHub::record_event(InputEventSnapshot event) {
    std::scoped_lock lock(mutex_);
    event.sequence = next_event_sequence_++;
    const std::uint64_t sequence = event.sequence;
    events_.push_back(std::move(event));
    while (events_.size() > max_events) {
        events_.pop_front();
    }
    return sequence;
}

void InspectionHub::publish(EditorStateSnapshot editor, ui::Scene scene, RenderStateSnapshot render,
                            std::uint64_t cause_event_sequence) {
    std::scoped_lock lock(mutex_);
    auto frame = std::make_shared<FrameInspection>();
    frame->frame_id = next_frame_id_++;
    frame->cause_event_sequence = cause_event_sequence;
    frame->editor = std::move(editor);
    frame->scene = std::move(scene);
    frame->render = std::move(render);
    frame->recent_events.assign(events_.begin(), events_.end());
    frame->violations = validate_frame(*frame);
    latest_ = std::move(frame);
}

std::shared_ptr<const FrameInspection> InspectionHub::latest() const {
    std::scoped_lock lock(mutex_);
    return latest_;
}

std::vector<InputEventSnapshot> InspectionHub::recent_events() const {
    std::scoped_lock lock(mutex_);
    return {events_.begin(), events_.end()};
}

std::string_view region_role_name(ui::RegionRole role) {
    switch (role) {
    case ui::RegionRole::TextArea:
        return "text-area";
    case ui::RegionRole::LineNumbers:
        return "line-numbers";
    case ui::RegionRole::ChangeSigns:
        return "change-signs";
    case ui::RegionRole::StatusBar:
        return "status-bar";
    case ui::RegionRole::EchoArea:
        return "echo-area";
    }
    return "unknown";
}

std::string_view style_class_name(ui::StyleClass style) {
    switch (style) {
    case ui::StyleClass::Text:
        return "text";
    case ui::StyleClass::Keyword:
        return "keyword";
    case ui::StyleClass::String:
        return "string";
    case ui::StyleClass::Number:
        return "number";
    case ui::StyleClass::Comment:
        return "comment";
    case ui::StyleClass::Preprocessor:
        return "preprocessor";
    case ui::StyleClass::Gutter:
        return "gutter";
    case ui::StyleClass::SignAdded:
        return "sign-added";
    case ui::StyleClass::SignModified:
        return "sign-modified";
    case ui::StyleClass::SignDeleted:
        return "sign-deleted";
    case ui::StyleClass::StatusBar:
        return "status-bar";
    case ui::StyleClass::StatusKey:
        return "status-key";
    case ui::StyleClass::Message:
        return "message";
    }
    return "unknown";
}

std::string inspection_snapshot_json(const FrameInspection& frame) {
    std::string output =
        std::format("{{\"schema\":{},\"frame_id\":{},\"cause_event_sequence\":{},\"editor\":",
                    FrameInspection::schema_version, frame.frame_id, frame.cause_event_sequence);
    append_editor(output, frame.editor);
    output += ",\"scene\":";
    append_scene(output, frame.scene);
    output += ",\"render\":";
    append_render(output, frame.render);
    output += ",\"recent_events\":";
    append_events(output, frame.recent_events);
    output += ",\"violations\":";
    append_strings(output, frame.violations);
    output.push_back('}');
    return output;
}

std::string inspection_tree_text(const FrameInspection& frame) {
    std::ostringstream output;
    output << "frame:" << frame.frame_id << " revision=" << frame.editor.revision
           << " cause-event=" << frame.cause_event_sequence << '\n';
    output << "  editor path=\"" << printable(frame.editor.path)
           << "\" dirty=" << (frame.editor.dirty ? "true" : "false")
           << " caret=" << frame.editor.caret_position.line << ':'
           << frame.editor.caret_display_column << " byte=" << frame.editor.caret.value << '\n';
    output << "  scene " << frame.scene.cols << 'x' << frame.scene.rows
           << " cursor=" << frame.scene.cursor_row << ':' << frame.scene.cursor_col << '\n';
    for (const ui::Region& region : frame.scene.regions) {
        output << "    region:" << region_role_name(region.role) << " rect=(" << region.rect.row
               << ',' << region.rect.col << ' ' << region.rect.rows << 'x' << region.rect.cols
               << ") prims=" << region.prims.size() << '\n';
        for (std::size_t index = 0; index < region.prims.size(); ++index) {
            const ui::Prim& prim = region.prims[index];
            output << "      prim:" << index << " cell=(" << prim.row << ',' << prim.col
                   << ") style=" << style_class_name(prim.style)
                   << (prim.selected ? " selected" : "") << " text=\"" << printable(prim.text)
                   << "\"\n";
        }
    }
    output << "  render driver=" << frame.render.video_driver
           << " scale=" << frame.render.display_scale << " window=" << frame.render.window_width
           << 'x' << frame.render.window_height << " output=" << frame.render.output_width << 'x'
           << frame.render.output_height << " cell=" << frame.render.cell_width << 'x'
           << frame.render.cell_height << " font=\"" << printable(frame.render.font_family)
           << "\"\n";
    output << "  violations=" << frame.violations.size() << '\n';
    for (const std::string& violation : frame.violations) {
        output << "    ! " << violation << '\n';
    }
    return output.str();
}

InspectionResponse run_inspection_query(const InspectionHub& hub, std::string_view request) {
    request = trim(request);
    if (request == "help") {
        return {
            true,
            "snapshot\ntree\nget <path>\npick <window-x> <window-y>\nevents [after-sequence]\n"};
    }
    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    if (!frame) {
        return {false, "no frame has been published"};
    }
    if (request == "snapshot") {
        return {true, inspection_snapshot_json(*frame)};
    }
    if (request == "tree") {
        return {true, inspection_tree_text(*frame)};
    }
    if (request == "get event.last_sequence") {
        const std::vector<InputEventSnapshot> events = hub.recent_events();
        return {true, std::to_string(events.empty() ? 0 : events.back().sequence)};
    }
    if (request == "events" || request.starts_with("events ")) {
        std::uint64_t after_sequence = 0;
        if (request.size() > std::string_view("events").size()) {
            std::istringstream input{std::string(trim(request.substr(6)))};
            if (!(input >> after_sequence)) {
                return {false, "usage: events [after-sequence]"};
            }
        }
        std::string output;
        append_events(output, hub.recent_events(), after_sequence);
        return {true, std::move(output)};
    }
    if (request.starts_with("get ")) {
        return get_query(*frame, request.substr(4));
    }
    if (request.starts_with("pick ")) {
        return pick_query(*frame, request.substr(5));
    }
    return {false, std::format("unknown inspection command '{}'", request)};
}

} // namespace cind::gui
