#include "gui/inspection.hpp"

#include "ui/char_width.hpp"
#include "ui/scene_layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

std::string_view surface_class_name(ui::SurfaceClass surface) {
    switch (surface) {
    case ui::SurfaceClass::Editor:
        return "editor";
    case ui::SurfaceClass::Gutter:
        return "gutter";
    case ui::SurfaceClass::Status:
        return "status";
    case ui::SurfaceClass::Echo:
        return "echo";
    }
    return "unknown";
}

std::string_view vertical_anchor_name(ui::VerticalAnchor anchor) {
    switch (anchor) {
    case ui::VerticalAnchor::Grid:
        return "grid";
    case ui::VerticalAnchor::Bottom:
        return "bottom";
    }
    return "unknown";
}

std::string_view prim_kind_name(ui::PrimKind kind) {
    switch (kind) {
    case ui::PrimKind::Text:
        return "text";
    case ui::PrimKind::ChangeBar:
        return "change-bar";
    case ui::PrimKind::ChangeDeletion:
        return "change-deletion";
    }
    return "unknown";
}

std::string primitive_id(const ui::Region& region, const ui::Prim& prim) {
    if (!prim.id.empty()) {
        return prim.id;
    }
    return std::format("region:{}/cell:{}:{}/{}", region_role_name(region.role), prim.row, prim.col,
                       prim_kind_name(prim.kind));
}

void append_rect(std::string& output, const ui::Rect& rect) {
    output += std::format("{{\"row\":{},\"col\":{},\"rows\":{},\"cols\":{}}}", rect.row, rect.col,
                          rect.rows, rect.cols);
}

void append_logical_rect(std::string& output, const LogicalPixelRectSnapshot& rect) {
    output += std::format("{{\"x\":{},\"y\":{},\"width\":{},\"height\":{}}}", rect.x, rect.y,
                          rect.width, rect.height);
}

void append_output_rect(std::string& output, const OutputPixelRectSnapshot& rect) {
    output += std::format("{{\"x\":{},\"y\":{},\"width\":{},\"height\":{}}}", rect.x, rect.y,
                          rect.width, rect.height);
}

void append_line_signs(std::string& output, const ui::LineSigns& signs) {
    output += std::format("{{\"first\":{},\"modified\":{},\"added\":{},\"deleted\":", signs.first,
                          signs.modified, signs.added);
    append_bool(output, signs.deleted);
    output += std::format(",\"boundary\":{}}}", signs.boundary);
}

void append_command_loop(std::string& output, const CommandLoopStateSnapshot& command_loop) {
    output += "{\"keymaps\":[";
    for (std::size_t index = 0; index < command_loop.keymaps.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_json_string(output, command_loop.keymaps[index]);
    }
    output += "],\"pending_keys\":";
    append_json_string(output, command_loop.pending_keys);
    output += ",\"pending_keymap\":";
    append_json_string(output, command_loop.pending_keymap);
    output += ",\"repeat_count\":";
    if (command_loop.repeat_count) {
        output += std::to_string(*command_loop.repeat_count);
    } else {
        output += "null";
    }
    output += ",\"last_command\":";
    append_json_string(output, command_loop.last_command);
    output += ",\"minibuffer\":{\"active\":";
    append_bool(output, command_loop.minibuffer.active);
    output += ",\"prompt\":";
    append_json_string(output, command_loop.minibuffer.prompt);
    output += ",\"input\":";
    append_json_string(output, command_loop.minibuffer.input);
    output += ",\"history\":";
    append_json_string(output, command_loop.minibuffer.history);
    output += ",\"completion_provider\":";
    append_json_string(output, command_loop.minibuffer.completion_provider);
    output += "}}";
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
    output += ",\"command_loop\":";
    append_command_loop(output, editor.command_loop);
    output += ",\"quit_armed\":";
    append_bool(output, editor.quit_armed);
    output += ",\"quit\":";
    append_bool(output, editor.quit);
    output.push_back('}');
}

void append_prim(std::string& output, const ui::Region& region, const ui::Prim& prim) {
    output += "{\"id\":";
    append_json_string(output, primitive_id(region, prim));
    output += std::format(",\"row\":{},\"col\":{},\"text\":", prim.row, prim.col);
    append_json_string(output, prim.text);
    output += ",\"style\":";
    append_json_string(output, style_class_name(prim.style));
    output += ",\"kind\":";
    append_json_string(output, prim_kind_name(prim.kind));
    output += ",\"selected\":";
    append_bool(output, prim.selected);
    output.push_back('}');
}

void append_region(std::string& output, const ui::Region& region) {
    output += "{\"id\":";
    append_json_string(output, std::format("region:{}", region_role_name(region.role)));
    output += ",\"role\":";
    append_json_string(output, region_role_name(region.role));
    output += ",\"surface\":";
    append_json_string(output, surface_class_name(region.surface));
    output += ",\"vertical_anchor\":";
    append_json_string(output, vertical_anchor_name(region.vertical_anchor));
    output += ",\"rect\":";
    append_rect(output, region.rect);
    output += ",\"prims\":[";
    for (std::size_t index = 0; index < region.prims.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_prim(output, region, region.prims[index]);
    }
    output += "]}";
}

void append_scene(std::string& output, const ui::Scene& scene) {
    output += std::format(
        "{{\"rows\":{},\"cols\":{},\"cursor\":{{\"row\":{},\"col\":{},\"visible\":", scene.rows,
        scene.cols, scene.cursor_row, scene.cursor_col);
    append_bool(output, scene.cursor_visible);
    output += "},\"regions\":[";
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

void append_font_metrics(std::string& output, const FontMetricsSnapshot& metrics) {
    output += std::format(
        "{{\"ascent\":{},\"descent\":{},\"leading\":{},\"baseline_from_row_top\":{}}}",
        metrics.ascent, metrics.descent, metrics.leading, metrics.baseline_from_row_top);
}

void append_render_primitive(std::string& output, const PrimitiveRenderSnapshot& primitive) {
    output += "{\"id\":";
    append_json_string(output, primitive.id);
    output += ",\"region\":";
    append_json_string(output, primitive.region);
    output += ",\"kind\":";
    append_json_string(output, primitive.kind);
    output +=
        std::format(",\"scene_index\":{{\"region\":{},\"primitive\":{}}},\"coordinate_space\":"
                    "\"logical-pixels\",\"cell_bounds\":",
                    primitive.region_index, primitive.primitive_index);
    append_logical_rect(output, primitive.cell_bounds);
    output += ",\"shape_bounds\":";
    if (primitive.shape_bounds) {
        append_logical_rect(output, *primitive.shape_bounds);
    } else {
        output += "null";
    }
    output += ",\"paint_bounds\":";
    if (primitive.paint_bounds) {
        append_logical_rect(output, *primitive.paint_bounds);
    } else {
        output += "null";
    }
    output += ",\"draw_bounds_cross_region_clip\":";
    append_bool(output, primitive.draw_bounds_cross_region_clip);
    output += ",\"row_overflow\":";
    append_bool(output, primitive.row_overflow);
    output += ",\"column_overflow\":";
    append_bool(output, primitive.column_overflow);
    output.push_back('}');
}

void append_render_primitives(std::string& output,
                              const std::vector<PrimitiveRenderSnapshot>& primitives) {
    output.push_back('[');
    for (std::size_t index = 0; index < primitives.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_render_primitive(output, primitives[index]);
    }
    output.push_back(']');
}

void append_render_damage(std::string& output, const RenderDamageSnapshot& damage) {
    output += "{\"full_repaint\":";
    append_bool(output, damage.full_repaint);
    output +=
        std::format(",\"damaged_cells\":{},\"damaged_output_pixels\":{},"
                    "\"output_fraction\":{},\"full_reference_match\":",
                    damage.damaged_cells, damage.damaged_output_pixels, damage.output_fraction);
    append_bool(output, damage.full_reference_match);
    output += ",\"rects\":[";
    for (std::size_t index = 0; index < damage.rects.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"logical\":";
        append_logical_rect(output, damage.rects[index].logical);
        output += ",\"output\":";
        append_output_rect(output, damage.rects[index].output);
        output.push_back('}');
    }
    output += "]}";
}

void append_render_animation(std::string& output, const RenderAnimationSnapshot& animation) {
    output += "{\"active\":";
    append_bool(output, animation.active);
    output += ",\"scroll\":";
    append_bool(output, animation.scroll);
    output += ",\"cursor\":";
    append_bool(output, animation.cursor);
    output +=
        std::format(",\"scroll_progress\":{},\"cursor_progress\":{},\"source_grid_offset_y\":{},"
                    "\"target_grid_offset_y\":{},\"cursor_rect\":",
                    animation.scroll_progress, animation.cursor_progress,
                    animation.source_grid_offset_y, animation.target_grid_offset_y);
    if (animation.cursor_rect) {
        append_logical_rect(output, *animation.cursor_rect);
    } else {
        output += "null";
    }
    output.push_back('}');
}

void append_render(std::string& output, const RenderStateSnapshot& render) {
    output += "{\"video_driver\":";
    append_json_string(output, render.video_driver);
    output += ",\"render_driver\":";
    append_json_string(output, render.render_driver);
    output += std::format(
        ",\"window\":{{\"width\":{},\"height\":{}}},\"output\":{{\"width\":{},\"height\":{}}},"
        "\"display_scale\":{},\"cell\":{{\"width\":{},\"height\":{}}},\"grid\":{{\"rows\":{},"
        "\"columns\":{}}},\"texture_format\":",
        render.window_width, render.window_height, render.output_width, render.output_height,
        render.display_scale, render.cell_width, render.cell_height, render.rows, render.columns);
    append_json_string(output, render.texture_format);
    output += ",\"font\":{\"family\":";
    append_json_string(output, render.font_family);
    output += std::format(",\"size\":{},\"metrics\":", render.font_size);
    append_font_metrics(output, render.font_metrics);
    output += "},\"theme\":{\"background\":";
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
    output += std::format("}},\"pixel_hash\":\"0x{:016X}\",\"animation\":", render.pixel_hash);
    append_render_animation(output, render.animation);
    output += ",\"damage\":";
    append_render_damage(output, render.damage);
    output += ",\"primitives\":";
    append_render_primitives(output, render.primitives);
    output.push_back('}');
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

void append_event_batch(std::string& output, const InspectionState& state,
                        std::uint64_t after_sequence) {
    const bool gap = after_sequence < state.oldest_event_sequence &&
                     state.oldest_event_sequence - after_sequence > 1;
    output += std::format("{{\"after_sequence\":{},\"oldest_available\":{},"
                          "\"last_sequence\":{},\"gap\":",
                          after_sequence, state.oldest_event_sequence, state.last_event_sequence);
    append_bool(output, gap);
    output += ",\"events\":";
    append_events(output, state.events, after_sequence);
    output.push_back('}');
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

void identify_render_primitives(const ui::Scene& scene, RenderStateSnapshot& render) {
    for (PrimitiveRenderSnapshot& primitive : render.primitives) {
        if (primitive.region_index >= scene.regions.size()) {
            continue;
        }
        const ui::Region& region = scene.regions[primitive.region_index];
        primitive.region = std::format("region:{}", region_role_name(region.role));
        if (primitive.primitive_index >= region.prims.size()) {
            continue;
        }
        const ui::Prim& scene_primitive = region.prims[primitive.primitive_index];
        primitive.id = primitive_id(region, scene_primitive);
        primitive.kind = std::string(prim_kind_name(scene_primitive.kind));
    }
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
    if (frame.scene.cursor_visible &&
        (frame.scene.cursor_row < 1 || frame.scene.cursor_row > frame.scene.rows ||
         frame.scene.cursor_col < 1 || frame.scene.cursor_col > frame.scene.cols)) {
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
    const RenderAnimationSnapshot& animation = frame.render.animation;
    if (animation.scroll_progress < 0.0F || animation.scroll_progress > 1.0F ||
        animation.cursor_progress < 0.0F || animation.cursor_progress > 1.0F) {
        violations.emplace_back("render animation progress is outside [0, 1]");
    }
    if (animation.active != (animation.scroll || animation.cursor)) {
        violations.emplace_back("render animation activity flags are inconsistent");
    }
    if (animation.active && !frame.render.damage.full_repaint) {
        violations.emplace_back("render animation requires a full presentation repaint");
    }
    if (!frame.render.damage.full_reference_match) {
        violations.emplace_back("retained raster differs from full reference render");
    }
    for (const RenderDamageRectSnapshot& damage : frame.render.damage.rects) {
        const OutputPixelRectSnapshot& rect = damage.output;
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
            rect.x + rect.width > frame.render.output_width ||
            rect.y + rect.height > frame.render.output_height) {
            violations.emplace_back("render damage rectangle is outside the output");
        }
    }
    for (const PrimitiveRenderSnapshot& primitive : frame.render.primitives) {
        if (primitive.region_index >= frame.scene.regions.size() ||
            primitive.primitive_index >= frame.scene.regions[primitive.region_index].prims.size()) {
            violations.emplace_back("render primitive refers to a missing scene primitive");
            continue;
        }
        if (primitive.row_overflow) {
            violations.push_back(
                std::format("render primitive '{}' crosses its scene row", primitive.id));
        }
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

const PrimitiveRenderSnapshot* find_render_primitive(const RenderStateSnapshot& render,
                                                     std::string_view id) {
    for (const PrimitiveRenderSnapshot& primitive : render.primitives) {
        if (primitive.id == id) {
            return &primitive;
        }
    }
    return nullptr;
}

const PrimitiveRenderSnapshot* find_render_primitive(const RenderStateSnapshot& render,
                                                     std::size_t region_index,
                                                     std::size_t primitive_index) {
    for (const PrimitiveRenderSnapshot& primitive : render.primitives) {
        if (primitive.region_index == region_index &&
            primitive.primitive_index == primitive_index) {
            return &primitive;
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
    } else if (path == "editor.command_loop") {
        append_command_loop(output, frame.editor.command_loop);
    } else if (path == "scene") {
        append_scene(output, frame.scene);
    } else if (path == "scene.cursor") {
        output = std::format("{{\"row\":{},\"col\":{},\"visible\":{}}}", frame.scene.cursor_row,
                             frame.scene.cursor_col, frame.scene.cursor_visible);
    } else if (path == "render") {
        append_render(output, frame.render);
    } else if (path == "render.font_metrics") {
        append_font_metrics(output, frame.render.font_metrics);
    } else if (path == "render.animation") {
        append_render_animation(output, frame.render.animation);
    } else if (path == "render.damage") {
        append_render_damage(output, frame.render.damage);
    } else if (path == "render.primitives") {
        append_render_primitives(output, frame.render.primitives);
    } else if (path.starts_with("render.primitive.")) {
        const std::string_view id = path.substr(std::string_view("render.primitive.").size());
        const PrimitiveRenderSnapshot* primitive = find_render_primitive(frame.render, id);
        if (!primitive) {
            return {false, std::format("unknown rendered primitive '{}'", id)};
        }
        append_render_primitive(output, *primitive);
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
        frame.render.cell_width <= 0 || frame.render.cell_height <= 0 || frame.scene.cols <= 0 ||
        frame.scene.rows <= 0) {
        return {false, "frame has no usable window geometry"};
    }
    if (window_x < 0.0F || window_y < 0.0F ||
        window_x >= static_cast<float>(frame.render.window_width) ||
        window_y >= static_cast<float>(frame.render.window_height)) {
        return {false, "point is outside the window"};
    }

    const float logical_width =
        static_cast<float>(frame.render.output_width) / frame.render.display_scale;
    const float logical_height =
        static_cast<float>(frame.render.output_height) / frame.render.display_scale;
    const float logical_x =
        window_x / static_cast<float>(frame.render.window_width) * logical_width;
    const float logical_y =
        window_y / static_cast<float>(frame.render.window_height) * logical_height;
    const int cell_col =
        static_cast<int>(std::floor(logical_x / static_cast<float>(frame.render.cell_width)));
    const ui::SceneVerticalLayout vertical_layout(
        frame.scene, {.cell_height = static_cast<float>(frame.render.cell_height),
                      .viewport_height = logical_height});
    const int cell_row = vertical_layout.row_at(logical_y);

    std::string output =
        std::format("{{\"window\":{{\"x\":{},\"y\":{}}},\"cell\":{{\"row\":{},\"col\":{}}}",
                    window_x, window_y, cell_row, cell_col);
    if (cell_row >= frame.scene.rows || cell_col >= frame.scene.cols) {
        output += ",\"region\":null,\"prim\":null,\"render\":null}";
        return {true, std::move(output)};
    }

    const ui::Region* hit_region = nullptr;
    std::size_t hit_region_index = frame.scene.regions.size();
    for (std::size_t index = 0; index < frame.scene.regions.size(); ++index) {
        const ui::Region& region = frame.scene.regions[index];
        if (cell_row >= region.rect.row && cell_row < region.rect.row + region.rect.rows &&
            cell_col >= region.rect.col && cell_col < region.rect.col + region.rect.cols) {
            hit_region = &region;
            hit_region_index = index;
            break;
        }
    }

    if (!hit_region) {
        output += ",\"region\":null,\"prim\":null,\"render\":null}";
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
    std::size_t hit_primitive_index = hit_region->prims.size();
    for (std::size_t index = 0; index < hit_region->prims.size(); ++index) {
        const ui::Prim& prim = hit_region->prims[index];
        const int width = std::max(1, ui::display_width(prim.text));
        if (prim.row == local_row && local_col >= prim.col && local_col < prim.col + width) {
            hit_prim = &prim;
            hit_primitive_index = index;
        }
    }
    output += ",\"prim\":";
    if (hit_prim) {
        append_prim(output, *hit_region, *hit_prim);
    } else {
        output += "null";
    }
    output += ",\"render\":";
    if (hit_prim) {
        if (const PrimitiveRenderSnapshot* rendered =
                find_render_primitive(frame.render, hit_region_index, hit_primitive_index)) {
            append_render_primitive(output, *rendered);
        } else {
            output += "null";
        }
    } else {
        output += "null";
    }
    output.push_back('}');
    return {true, std::move(output)};
}

} // namespace

std::uint64_t InspectionHub::record_event(InputEventSnapshot event) {
    std::uint64_t sequence = 0;
    {
        std::scoped_lock lock(mutex_);
        event.sequence = next_event_sequence_++;
        sequence = event.sequence;
        events_.push_back(std::move(event));
        while (events_.size() > max_events) {
            events_.pop_front();
        }
    }
    changed_.notify_all();
    return sequence;
}

void InspectionHub::publish(EditorStateSnapshot editor, ui::Scene scene, RenderStateSnapshot render,
                            std::uint64_t cause_event_sequence) {
    identify_render_primitives(scene, render);
    {
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
    changed_.notify_all();
}

std::shared_ptr<const FrameInspection> InspectionHub::latest() const {
    std::scoped_lock lock(mutex_);
    return latest_;
}

std::vector<InputEventSnapshot> InspectionHub::recent_events() const {
    std::scoped_lock lock(mutex_);
    return {events_.begin(), events_.end()};
}

InspectionState InspectionHub::snapshot() const {
    std::scoped_lock lock(mutex_);
    return {.frame = latest_,
            .events = {events_.begin(), events_.end()},
            .oldest_event_sequence =
                events_.empty() ? next_event_sequence_ : events_.front().sequence,
            .last_event_sequence = next_event_sequence_ - 1};
}

InspectionState InspectionHub::wait_for_frame(std::uint64_t after_frame,
                                              std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, timeout, [&] { return latest_ && latest_->frame_id > after_frame; });
    return {.frame = latest_,
            .events = {events_.begin(), events_.end()},
            .oldest_event_sequence =
                events_.empty() ? next_event_sequence_ : events_.front().sequence,
            .last_event_sequence = next_event_sequence_ - 1};
}

InspectionState InspectionHub::wait_for_events(std::uint64_t after_event,
                                               std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, timeout, [&] { return next_event_sequence_ - 1 > after_event; });
    return {.frame = latest_,
            .events = {events_.begin(), events_.end()},
            .oldest_event_sequence =
                events_.empty() ? next_event_sequence_ : events_.front().sequence,
            .last_event_sequence = next_event_sequence_ - 1};
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
    output << "    command keymaps=" << frame.editor.command_loop.keymaps.size() << " pending=\""
           << printable(frame.editor.command_loop.pending_keys) << "\" owner=\""
           << printable(frame.editor.command_loop.pending_keymap) << "\" last=\""
           << printable(frame.editor.command_loop.last_command) << "\" minibuffer="
           << (frame.editor.command_loop.minibuffer.active ? "active" : "inactive") << '\n';
    output << "  scene " << frame.scene.cols << 'x' << frame.scene.rows << " cursor=";
    if (frame.scene.cursor_visible) {
        output << frame.scene.cursor_row << ':' << frame.scene.cursor_col;
    } else {
        output << "hidden";
    }
    output << '\n';
    for (const ui::Region& region : frame.scene.regions) {
        output << "    region:" << region_role_name(region.role) << " rect=(" << region.rect.row
               << ',' << region.rect.col << ' ' << region.rect.rows << 'x' << region.rect.cols
               << ") anchor=" << vertical_anchor_name(region.vertical_anchor)
               << " prims=" << region.prims.size() << '\n';
        for (std::size_t index = 0; index < region.prims.size(); ++index) {
            const ui::Prim& prim = region.prims[index];
            output << "      prim:" << (prim.id.empty() ? std::to_string(index) : prim.id)
                   << " cell=(" << prim.row << ',' << prim.col
                   << ") kind=" << prim_kind_name(prim.kind)
                   << " style=" << style_class_name(prim.style)
                   << (prim.selected ? " selected" : "") << " text=\"" << printable(prim.text)
                   << "\"\n";
        }
    }
    output << "  render video-driver=" << frame.render.video_driver
           << " render-driver=" << frame.render.render_driver
           << " scale=" << frame.render.display_scale << " window=" << frame.render.window_width
           << 'x' << frame.render.window_height << " output=" << frame.render.output_width << 'x'
           << frame.render.output_height << " cell=" << frame.render.cell_width << 'x'
           << frame.render.cell_height << " font=\"" << printable(frame.render.font_family)
           << "\" baseline=" << frame.render.font_metrics.baseline_from_row_top << '\n';
    std::size_t row_overflows = 0;
    std::size_t draw_bounds_clip_crossings = 0;
    for (const PrimitiveRenderSnapshot& primitive : frame.render.primitives) {
        row_overflows += primitive.row_overflow ? 1 : 0;
        draw_bounds_clip_crossings += primitive.draw_bounds_cross_region_clip ? 1 : 0;
    }
    output << "    primitives=" << frame.render.primitives.size()
           << " row-overflows=" << row_overflows
           << " draw-bounds-cross-clip=" << draw_bounds_clip_crossings << '\n';
    output << "    animation=" << (frame.render.animation.active ? "active" : "idle")
           << " scroll=" << (frame.render.animation.scroll ? "true" : "false")
           << " cursor=" << (frame.render.animation.cursor ? "true" : "false")
           << " scroll-progress=" << frame.render.animation.scroll_progress
           << " cursor-progress=" << frame.render.animation.cursor_progress << '\n';
    output << "    damage=" << (frame.render.damage.full_repaint ? "full" : "partial")
           << " rects=" << frame.render.damage.rects.size()
           << " cells=" << frame.render.damage.damaged_cells
           << " output-pixels=" << frame.render.damage.damaged_output_pixels
           << " fraction=" << frame.render.damage.output_fraction
           << " reference-match=" << (frame.render.damage.full_reference_match ? "true" : "false")
           << '\n';
    for (const PrimitiveRenderSnapshot& primitive : frame.render.primitives) {
        if (primitive.row_overflow) {
            output << "      ! " << printable(primitive.id) << " cell-y=" << primitive.cell_bounds.y
                   << ".." << primitive.cell_bounds.y + primitive.cell_bounds.height;
            if (primitive.paint_bounds) {
                output << " paint-y=" << primitive.paint_bounds->y << ".."
                       << primitive.paint_bounds->y + primitive.paint_bounds->height;
            }
            output << '\n';
        }
    }
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
            "snapshot\nsnapshot-after <frame-id>\ntree\nget <path>\npick <window-x> <window-y>\n"
            "events [after-sequence]\nwait-frame <frame-id>\nwait-events <event-sequence>\n"};
    }
    InspectionState state;
    if (request.starts_with("wait-frame ")) {
        std::uint64_t after = 0;
        std::istringstream input{std::string(trim(request.substr(11)))};
        if (!(input >> after)) {
            return {false, "usage: wait-frame <frame-id>"};
        }
        state = hub.wait_for_frame(after, std::chrono::seconds(1));
    } else if (request.starts_with("wait-events ")) {
        std::uint64_t after = 0;
        std::istringstream input{std::string(trim(request.substr(12)))};
        if (!(input >> after)) {
            return {false, "usage: wait-events <event-sequence>"};
        }
        state = hub.wait_for_events(after, std::chrono::seconds(1));
        std::string output;
        append_event_batch(output, state, after);
        return {true, std::move(output)};
    } else {
        state = hub.snapshot();
    }
    const std::shared_ptr<const FrameInspection>& frame = state.frame;
    if (!frame) {
        return {false, "no frame has been published"};
    }
    if (request.starts_with("wait-frame ")) {
        std::uint64_t after = 0;
        std::istringstream{std::string(trim(request.substr(11)))} >> after;
        return {true, frame->frame_id > after ? inspection_snapshot_json(*frame) : std::string()};
    }
    if (request == "snapshot") {
        return {true, inspection_snapshot_json(*frame)};
    }
    if (request.starts_with("snapshot-after ")) {
        std::uint64_t after = 0;
        std::istringstream input{std::string(trim(request.substr(15)))};
        if (!(input >> after)) {
            return {false, "usage: snapshot-after <frame-id>"};
        }
        return {true, frame->frame_id > after ? inspection_snapshot_json(*frame) : std::string()};
    }
    if (request == "tree") {
        return {true, inspection_tree_text(*frame)};
    }
    if (request == "get event.last_sequence") {
        return {true, std::to_string(state.last_event_sequence)};
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
        append_event_batch(output, state, after_sequence);
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
