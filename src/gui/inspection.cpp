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

void append_strings(std::string& output, const std::vector<std::string>& values);

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
    case ui::VerticalAnchor::Overlay:
        return "overlay";
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

void append_logical_point(std::string& output, const LogicalPixelPointSnapshot& point) {
    output += std::format("{{\"x\":{},\"y\":{}}}", point.x, point.y);
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
    output += "],\"layers\":[";
    for (std::size_t index = 0; index < command_loop.layers.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"name\":";
        append_json_string(output, command_loop.layers[index].name);
        output += ",\"scope\":";
        append_json_string(output, command_loop.layers[index].scope);
        output += ",\"parents\":";
        append_strings(output, command_loop.layers[index].parents);
        output.push_back('}');
    }
    output += "],\"override_keymaps\":";
    append_strings(output, command_loop.override_keymaps);
    output += ",\"pending_keys\":";
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
    output.push_back('}');
}

void append_interaction(std::string& output, const InteractionStateSnapshot& interaction) {
    output += "{\"active\":";
    append_bool(output, interaction.active);
    output += ",\"kind\":";
    append_json_string(output, interaction.kind);
    output += ",\"prompt\":";
    append_json_string(output, interaction.prompt);
    output += ",\"input\":";
    append_json_string(output, interaction.input);
    output += std::format(",\"input_cursor\":{}", interaction.input_cursor);
    output += ",\"history\":";
    append_json_string(output, interaction.history);
    output += ",\"provider\":";
    append_json_string(output, interaction.provider);
    output += ",\"allow_custom_input\":";
    append_bool(output, interaction.allow_custom_input);
    output += std::format(",\"generation\":{},\"selected\":{},\"error\":", interaction.generation,
                          interaction.selected);
    append_json_string(output, interaction.error);
    output += ",\"candidates\":[";
    for (std::size_t index = 0; index < interaction.candidates.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const InteractionCandidateSnapshot& candidate = interaction.candidates[index];
        output += "{\"value\":";
        append_json_string(output, candidate.value);
        output += ",\"label\":";
        append_json_string(output, candidate.label);
        output += ",\"detail\":";
        append_json_string(output, candidate.detail);
        output.push_back('}');
    }
    output += "]}";
}

void append_buffers(std::string& output, const std::vector<OpenBufferStateSnapshot>& buffers) {
    output.push_back('[');
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const OpenBufferStateSnapshot& buffer = buffers[index];
        output += std::format("{{\"buffer\":{{\"slot\":{},\"generation\":{}}},\"view\":",
                              buffer.buffer_slot, buffer.buffer_generation);
        if (buffer.view_present) {
            output += std::format("{{\"slot\":{},\"generation\":{}}}", buffer.view_slot,
                                  buffer.view_generation);
        } else {
            output += "null";
        }
        output += ",\"name\":";
        append_json_string(output, buffer.name);
        output += ",\"resource\":";
        append_json_string(output, buffer.resource);
        output += ",\"modified\":";
        append_bool(output, buffer.modified);
        output += ",\"active\":";
        append_bool(output, buffer.active);
        output += ",\"saving\":";
        append_bool(output, buffer.saving);
        output.push_back('}');
    }
    output.push_back(']');
}

void append_windows(std::string& output, const std::vector<OpenWindowStateSnapshot>& windows) {
    output.push_back('[');
    for (std::size_t index = 0; index < windows.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const OpenWindowStateSnapshot& window = windows[index];
        output += std::format("{{\"window\":{{\"slot\":{},\"generation\":{}}},"
                              "\"view\":{{\"slot\":{},\"generation\":{}}},"
                              "\"buffer\":{{\"slot\":{},\"generation\":{}}},\"active\":",
                              window.window_slot, window.window_generation, window.view_slot,
                              window.view_generation, window.buffer_slot, window.buffer_generation);
        append_bool(output, window.active);
        output.push_back('}');
    }
    output.push_back(']');
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
    output += std::format(
        ",\"viewport\":{{\"top_line\":{},\"top_line_offset\":{},\"left_column\":{}}},"
        "\"line_signs\":",
        editor.viewport.top_line, editor.viewport.top_line_offset, editor.viewport.left_column);
    append_line_signs(output, editor.line_signs);
    output += std::format(",\"tab_width\":{},\"style_origin\":", editor.tab_width);
    append_json_string(output, editor.style_origin);
    output += ",\"message\":";
    append_json_string(output, editor.message);
    output += ",\"preedit\":";
    append_json_string(output, editor.preedit);
    output += ",\"last_key\":";
    append_json_string(output, editor.last_key);
    output += std::format(",\"active_window\":{{\"slot\":{},\"generation\":{}}},\"input_focus\":",
                          editor.active_window_slot, editor.active_window_generation);
    append_json_string(output, editor.input_focus);
    output += ",\"command_loop\":";
    append_command_loop(output, editor.command_loop);
    output += ",\"interaction\":";
    append_interaction(output, editor.interaction);
    output += ",\"buffers\":";
    append_buffers(output, editor.buffers);
    output += ",\"windows\":";
    append_windows(output, editor.windows);
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
    output += ",\"popup\":";
    if (region.popup) {
        output += "{\"title\":";
        append_json_string(output, region.popup->title);
        output += ",\"input\":";
        if (region.popup->input) {
            append_json_string(output, *region.popup->input);
        } else {
            output += "null";
        }
        output += ",\"input_cursor\":";
        if (region.popup->input_cursor) {
            output += std::to_string(*region.popup->input_cursor);
        } else {
            output += "null";
        }
        output += std::format(",\"first_item\":{},\"total_items\":{}", region.popup->first_item,
                              region.popup->total_items);
        output += ",\"selected_item\":";
        if (region.popup->selected_item) {
            output += std::to_string(*region.popup->selected_item);
        } else {
            output += "null";
        }
        output += ",\"items\":[";
        for (std::size_t index = 0; index < region.popup->items.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            output += "{\"label\":";
            append_json_string(output, region.popup->items[index].label);
            output += ",\"detail\":";
            append_json_string(output, region.popup->items[index].detail);
            output.push_back('}');
        }
        output += "]}";
    } else {
        output += "null";
    }
    output += ",\"status\":";
    if (region.status) {
        output += "{\"path\":";
        append_json_string(output, region.status->path);
        output += ",\"dirty\":";
        append_bool(output, region.status->dirty);
        output += std::format(",\"line\":{},\"column\":{},\"line_count\":{},\"revision\":{}",
                              region.status->line, region.status->column, region.status->line_count,
                              region.status->revision);
        output += ",\"style_origin\":";
        append_json_string(output, region.status->style_origin);
        output += ",\"key\":";
        append_json_string(output, region.status->key);
        output.push_back('}');
    } else {
        output += "null";
    }
    output += ",\"echo\":";
    if (region.echo) {
        output += "{\"text\":";
        append_json_string(output, region.echo->text);
        output += ",\"cursor_byte\":";
        if (region.echo->cursor_byte) {
            output += std::to_string(*region.echo->cursor_byte);
        } else {
            output += "null";
        }
        output.push_back('}');
    } else {
        output += "null";
    }
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
    output += std::format("{{\"rows\":{},\"cols\":{},\"grid_offset_rows\":{},\"active_text_row\":",
                          scene.rows, scene.cols, scene.grid_offset_rows);
    if (scene.active_text_row) {
        output += std::to_string(*scene.active_text_row);
    } else {
        output += "null";
    }
    output += std::format(",\"cursor\":{{\"row\":{},\"col\":{},\"visible\":", scene.cursor_row,
                          scene.cursor_col);
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
                    "\"logical-pixels\",\"layout_bounds\":",
                    primitive.region_index, primitive.primitive_index);
    append_logical_rect(output, primitive.layout_bounds);
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

void append_text_layout(std::string& output, const TextLayoutSnapshot& text) {
    output += "{\"role\":";
    append_json_string(output, text.role);
    output +=
        std::format(",\"byte_count\":{},\"advance\":{},\"origin\":", text.byte_count, text.advance);
    append_logical_point(output, text.origin);
    output += ",\"shape_bounds\":";
    if (text.shape_bounds) {
        append_logical_rect(output, *text.shape_bounds);
    } else {
        output += "null";
    }
    output.push_back('}');
}

void append_popup_layout(std::string& output, const std::optional<PopupLayoutSnapshot>& popup) {
    if (!popup) {
        output += "null";
        return;
    }
    output += "{\"coordinate_space\":\"logical-pixels\",\"panel_bounds\":";
    append_logical_rect(output, popup->panel_bounds);
    output += ",\"header_bounds\":";
    append_logical_rect(output, popup->header_bounds);
    output += std::format(",\"horizontal_scroll\":{},\"input_bytes\":{},\"input_cursor\":{},"
                          "\"cursor_advance\":{},\"unclamped_cursor_x\":{},\"cursor_clamped\":",
                          popup->horizontal_scroll, popup->input_bytes, popup->input_cursor,
                          popup->cursor_advance, popup->unclamped_cursor_x);
    append_bool(output, popup->cursor_clamped);
    output += ",\"cursor_rect\":";
    if (popup->cursor_rect) {
        append_logical_rect(output, *popup->cursor_rect);
    } else {
        output += "null";
    }
    output += ",\"header_text\":[";
    for (std::size_t index = 0; index < popup->header_text.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_text_layout(output, popup->header_text[index]);
    }
    output += "]}";
}

void append_echo_layout(std::string& output, const std::optional<EchoLayoutSnapshot>& echo) {
    if (!echo) {
        output += "null";
        return;
    }
    output += "{\"coordinate_space\":\"logical-pixels\",\"bounds\":";
    append_logical_rect(output, echo->bounds);
    output += std::format(",\"horizontal_scroll\":{},\"text_bytes\":{},\"cursor_byte\":",
                          echo->horizontal_scroll, echo->text_bytes);
    if (echo->cursor_byte) {
        output += std::to_string(*echo->cursor_byte);
    } else {
        output += "null";
    }
    output += std::format(",\"cursor_advance\":{},\"unclamped_cursor_x\":{},\"cursor_clamped\":",
                          echo->cursor_advance, echo->unclamped_cursor_x);
    append_bool(output, echo->cursor_clamped);
    output += ",\"cursor_rect\":";
    if (echo->cursor_rect) {
        append_logical_rect(output, *echo->cursor_rect);
    } else {
        output += "null";
    }
    output += ",\"text\":";
    append_text_layout(output, echo->text);
    output.push_back('}');
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
        std::format(",\"scroll_progress\":{},\"cursor_progress\":{},\"scroll_velocity\":{},"
                    "\"source_grid_offset_y\":{},\"target_grid_offset_y\":{},\"cursor_rect\":",
                    animation.scroll_progress, animation.cursor_progress, animation.scroll_velocity,
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
    output += "},\"theme\":{\"canvas\":";
    append_color(output, render.theme.canvas);
    output += ",\"surface\":";
    append_color(output, render.theme.surface);
    output += ",\"raised\":";
    append_color(output, render.theme.raised);
    output += ",\"hairline\":";
    append_color(output, render.theme.hairline);
    output += ",\"active_line\":";
    append_color(output, render.theme.active_line);
    output += ",\"selection\":";
    append_color(output, render.theme.selection);
    output += ",\"text\":";
    append_color(output, render.theme.text);
    output += ",\"strong\":";
    append_color(output, render.theme.strong);
    output += ",\"muted\":";
    append_color(output, render.theme.muted);
    output += ",\"faint\":";
    append_color(output, render.theme.faint);
    output += ",\"accent\":";
    append_color(output, render.theme.accent);
    output += ",\"shadow\":";
    append_color(output, render.theme.shadow);
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
    output += ",\"popup_layout\":";
    append_popup_layout(output, render.popup_layout);
    output += ",\"echo_layout\":";
    append_echo_layout(output, render.echo_layout);
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
    if (!std::isfinite(frame.editor.viewport.top_line_offset) ||
        frame.editor.viewport.top_line_offset < 0.0F ||
        frame.editor.viewport.top_line_offset >= 1.0F) {
        violations.emplace_back("editor viewport line offset is outside [0, 1)");
    }
    const std::size_t active_windows = static_cast<std::size_t>(std::ranges::count_if(
        frame.editor.windows, [](const OpenWindowStateSnapshot& window) { return window.active; }));
    const OpenWindowStateSnapshot* active_window = nullptr;
    if (frame.editor.windows.empty() || active_windows != 1) {
        violations.emplace_back("editor must have exactly one active window");
    } else {
        const auto active =
            std::ranges::find_if(frame.editor.windows, [](const OpenWindowStateSnapshot& window) {
                return window.active;
            });
        active_window = &*active;
        if (active->window_slot != frame.editor.active_window_slot ||
            active->window_generation != frame.editor.active_window_generation) {
            violations.emplace_back("editor active window identity does not match window state");
        }
    }
    const std::size_t active_buffers = static_cast<std::size_t>(std::ranges::count_if(
        frame.editor.buffers, [](const OpenBufferStateSnapshot& buffer) { return buffer.active; }));
    if (frame.editor.buffers.empty() || active_buffers != 1) {
        violations.emplace_back("editor must have exactly one active buffer");
    } else if (active_window != nullptr) {
        const auto active_buffer =
            std::ranges::find_if(frame.editor.buffers, [](const OpenBufferStateSnapshot& buffer) {
                return buffer.active;
            });
        if (active_buffer->buffer_slot != active_window->buffer_slot ||
            active_buffer->buffer_generation != active_window->buffer_generation ||
            !active_buffer->view_present || active_buffer->view_slot != active_window->view_slot ||
            active_buffer->view_generation != active_window->view_generation) {
            violations.emplace_back("active window, view, and buffer bindings do not agree");
        }
    }
    const std::string_view expected_focus = frame.editor.interaction.active
                                                ? std::string_view("interaction")
                                                : std::string_view("window");
    if (frame.editor.input_focus != expected_focus) {
        violations.emplace_back("editor input focus does not match interaction state");
    }
    if (frame.editor.interaction.input_cursor > frame.editor.interaction.input.size()) {
        violations.emplace_back("interaction input cursor is past the input end");
    }
    if (frame.editor.command_loop.keymaps.size() != frame.editor.command_loop.layers.size() ||
        !std::ranges::equal(frame.editor.command_loop.keymaps, frame.editor.command_loop.layers,
                            [](const std::string& keymap, const KeymapLayerStateSnapshot& layer) {
                                return keymap == layer.name;
                            })) {
        violations.emplace_back("command keymap names do not match layer state");
    }
    if (frame.editor.command_loop.layers.empty() ||
        (expected_focus == "interaction") !=
            (frame.editor.command_loop.layers.front().scope == "interaction")) {
        violations.emplace_back("command keymap layers do not match input focus");
    }
    if (frame.editor.command_loop.layers.empty() ||
        frame.editor.command_loop.layers.back().scope != "global") {
        violations.emplace_back("command keymap layers have no global fallback");
    }
    if (frame.scene.rows <= 0 || frame.scene.cols <= 0) {
        violations.emplace_back("scene geometry must be positive");
    }
    if (!std::isfinite(frame.scene.grid_offset_rows) || frame.scene.grid_offset_rows > 0.0F ||
        frame.scene.grid_offset_rows <= -1.0F) {
        violations.emplace_back("scene grid offset is outside (-1, 0]");
    }
    if (std::abs(frame.scene.grid_offset_rows + frame.editor.viewport.top_line_offset) > 0.0001F) {
        violations.emplace_back("scene grid offset does not match editor viewport");
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
        (void)region.popup.transform([&](const ui::Region::PopupContent& popup) {
            if (popup.input_cursor && (!popup.input || *popup.input_cursor > popup.input->size())) {
                violations.emplace_back("popup input cursor is past the input end");
            }
            if (popup.first_item > popup.total_items ||
                popup.items.size() > popup.total_items - popup.first_item) {
                violations.emplace_back("popup viewport is outside the item range");
            }
            if (popup.selected_item && *popup.selected_item >= popup.total_items) {
                violations.emplace_back("popup selection is outside the item range");
            } else if (popup.selected_item &&
                       (*popup.selected_item < popup.first_item ||
                        *popup.selected_item >= popup.first_item + popup.items.size())) {
                violations.emplace_back("popup selection is outside the visible viewport");
            }
            if (region.prims.size() != popup.items.size() + 1) {
                violations.emplace_back("popup items do not match scene primitives");
            } else {
                for (std::size_t index = 0; index < popup.items.size(); ++index) {
                    const bool selected = popup.selected_item == popup.first_item + index;
                    if (region.prims[index + 1].selected != selected) {
                        violations.emplace_back("popup selection does not match scene primitives");
                        break;
                    }
                }
            }
            return true;
        });
        (void)region.echo.transform([&](const ui::Region::EchoContent& echo) {
            if (region.role != ui::RegionRole::EchoArea) {
                violations.emplace_back("structured echo content is outside the echo region");
            }
            if (echo.cursor_byte && *echo.cursor_byte > echo.text.size()) {
                violations.emplace_back("echo cursor byte is past the text end");
            }
            if (region.prims.empty() || region.prims.front().text != echo.text) {
                violations.emplace_back("echo content does not match scene primitives");
            }
            return true;
        });
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
    const ui::Region* popup_region = frame.scene.find(ui::RegionRole::Popup);
    const ui::Region::PopupContent* popup =
        popup_region != nullptr && popup_region->popup ? &*popup_region->popup : nullptr;
    if (popup != nullptr && !frame.render.popup_layout) {
        violations.emplace_back("scene popup has no render popup layout");
    }
    if (frame.render.popup_layout) {
        const PopupLayoutSnapshot& layout = *frame.render.popup_layout;
        if (popup == nullptr) {
            violations.emplace_back("render popup layout has no scene popup");
        } else {
            const std::size_t input_bytes = popup->input ? popup->input->size() : 0;
            const std::size_t input_cursor =
                popup->input ? popup->input_cursor.value_or(input_bytes) : 0;
            if (layout.input_bytes != input_bytes || layout.input_cursor != input_cursor) {
                violations.emplace_back("render popup input does not match scene popup");
            }
            const auto input_text =
                std::ranges::find_if(layout.header_text, [](const TextLayoutSnapshot& text) {
                    return text.role == "input";
                });
            if (popup->input) {
                if (input_text == layout.header_text.end() ||
                    input_text->byte_count != popup->input->size()) {
                    violations.emplace_back("render popup has no matching input text layout");
                }
                if (!layout.cursor_rect) {
                    violations.emplace_back("render popup input has no cursor layout");
                }
                if (input_text != layout.header_text.end() &&
                    std::abs(input_text->origin.x + layout.cursor_advance -
                             layout.unclamped_cursor_x) > 0.01F) {
                    violations.emplace_back("render popup cursor advance disagrees with input");
                }
            } else if (input_text != layout.header_text.end() || layout.cursor_rect) {
                violations.emplace_back("non-input popup has interactive text layout");
            }
        }
        if (layout.horizontal_scroll > 0.01F) {
            violations.emplace_back("render popup horizontal scroll is positive");
        }
        const float panel_right = layout.panel_bounds.x + layout.panel_bounds.width;
        const float panel_bottom = layout.panel_bounds.y + layout.panel_bounds.height;
        const float header_right = layout.header_bounds.x + layout.header_bounds.width;
        const float header_bottom = layout.header_bounds.y + layout.header_bounds.height;
        if (layout.header_bounds.x < layout.panel_bounds.x - 0.01F ||
            layout.header_bounds.y < layout.panel_bounds.y - 0.01F ||
            header_right > panel_right + 0.01F || header_bottom > panel_bottom + 0.01F) {
            violations.emplace_back("render popup header is outside its panel");
        }
        if (layout.cursor_rect) {
            const float cursor_right = layout.cursor_rect->x + layout.cursor_rect->width;
            const float cursor_bottom = layout.cursor_rect->y + layout.cursor_rect->height;
            if (layout.cursor_rect->x < layout.header_bounds.x - 0.01F ||
                layout.cursor_rect->y < layout.header_bounds.y - 0.01F ||
                cursor_right > header_right + 0.01F || cursor_bottom > header_bottom + 0.01F) {
                violations.emplace_back("render popup cursor is outside its header");
            }
            if (!layout.cursor_clamped &&
                std::abs(layout.cursor_rect->x - layout.unclamped_cursor_x) > 0.01F) {
                violations.emplace_back("render popup cursor does not match shaped advance");
            }
        }
        for (const TextLayoutSnapshot& text : layout.header_text) {
            if (!std::isfinite(text.advance) || text.advance < 0.0F ||
                !std::isfinite(text.origin.x) || !std::isfinite(text.origin.y)) {
                violations.emplace_back("render popup text layout is not finite");
                break;
            }
        }
    }
    const ui::Region* echo_region = frame.scene.find(ui::RegionRole::EchoArea);
    const ui::Region::EchoContent* echo =
        echo_region != nullptr && echo_region->echo ? &*echo_region->echo : nullptr;
    const bool popup_owns_input = popup != nullptr && popup->input.has_value();
    if (echo != nullptr && !popup_owns_input && !frame.render.echo_layout) {
        violations.emplace_back("scene echo has no render echo layout");
    }
    if (popup_owns_input && frame.render.echo_layout) {
        violations.emplace_back("popup input and render echo both own the cursor");
    }
    if (frame.render.echo_layout) {
        const EchoLayoutSnapshot& layout = *frame.render.echo_layout;
        if (echo == nullptr) {
            violations.emplace_back("render echo layout has no scene echo");
        } else {
            if (layout.text_bytes != echo->text.size() ||
                layout.text.byte_count != echo->text.size()) {
                violations.emplace_back("render echo text does not match scene echo");
            }
            if (layout.cursor_byte != echo->cursor_byte) {
                violations.emplace_back("render echo cursor does not match scene echo");
            }
            if (echo->cursor_byte) {
                if (!layout.cursor_rect) {
                    violations.emplace_back("render echo input has no cursor layout");
                }
                if (std::abs(layout.text.origin.x + layout.cursor_advance -
                             layout.unclamped_cursor_x) > 0.01F) {
                    violations.emplace_back("render echo cursor advance disagrees with text");
                }
            } else if (layout.cursor_rect) {
                violations.emplace_back("non-input echo has interactive cursor layout");
            }
        }
        if (layout.horizontal_scroll > 0.01F) {
            violations.emplace_back("render echo horizontal scroll is positive");
        }
        if (!std::isfinite(layout.text.advance) || layout.text.advance < 0.0F ||
            !std::isfinite(layout.text.origin.x) || !std::isfinite(layout.text.origin.y)) {
            violations.emplace_back("render echo text layout is not finite");
        }
        if (layout.cursor_rect) {
            const float bounds_right = layout.bounds.x + layout.bounds.width;
            const float bounds_bottom = layout.bounds.y + layout.bounds.height;
            const float cursor_right = layout.cursor_rect->x + layout.cursor_rect->width;
            const float cursor_bottom = layout.cursor_rect->y + layout.cursor_rect->height;
            if (layout.cursor_rect->x < layout.bounds.x - 0.01F ||
                layout.cursor_rect->y < layout.bounds.y - 0.01F ||
                cursor_right > bounds_right + 0.01F || cursor_bottom > bounds_bottom + 0.01F) {
                violations.emplace_back("render echo cursor is outside its bounds");
            }
            if (!layout.cursor_clamped &&
                std::abs(layout.cursor_rect->x - layout.unclamped_cursor_x) > 0.01F) {
                violations.emplace_back("render echo cursor does not match shaped advance");
            }
        }
    }
    const ui::Region* text_area = frame.scene.find(ui::RegionRole::TextArea);
    if (!frame.render.animation.scroll && frame.scene.cursor_visible && text_area) {
        const int cursor_row = frame.scene.cursor_row - 1;
        if (cursor_row >= text_area->rect.row &&
            cursor_row < text_area->rect.row + text_area->rect.rows &&
            frame.render.display_scale > 0.0F && frame.render.cell_height > 0) {
            const float logical_height =
                static_cast<float>(frame.render.output_height) / frame.render.display_scale;
            const ui::SceneVerticalLayout layout(
                frame.scene, {.cell_height = static_cast<float>(frame.render.cell_height),
                              .viewport_height = logical_height,
                              .footer_heights = ui::editor_footer_heights(
                                  static_cast<float>(frame.render.cell_height))});
            const float top = layout.row_top(cursor_row);
            const float bottom = top + static_cast<float>(frame.render.cell_height);
            if (top < -0.01F || bottom > layout.grid_clip_bottom() + 0.01F) {
                violations.emplace_back("rendered text cursor row is clipped");
            }
        }
    }
    const RenderAnimationSnapshot& animation = frame.render.animation;
    if (animation.scroll_progress < 0.0F || animation.scroll_progress > 1.0F ||
        animation.cursor_progress < 0.0F || animation.cursor_progress > 1.0F) {
        violations.emplace_back("render animation progress is outside [0, 1]");
    }
    if (!std::isfinite(animation.scroll_velocity)) {
        violations.emplace_back("render scroll velocity is not finite");
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
        output = std::format("{{\"top_line\":{},\"top_line_offset\":{},\"left_column\":{}}}",
                             frame.editor.viewport.top_line, frame.editor.viewport.top_line_offset,
                             frame.editor.viewport.left_column);
    } else if (path == "editor.line_signs") {
        append_line_signs(output, frame.editor.line_signs);
    } else if (path == "editor.command_loop") {
        append_command_loop(output, frame.editor.command_loop);
    } else if (path == "editor.interaction") {
        append_interaction(output, frame.editor.interaction);
    } else if (path == "editor.buffers") {
        append_buffers(output, frame.editor.buffers);
    } else if (path == "editor.windows") {
        append_windows(output, frame.editor.windows);
    } else if (path == "editor.focus") {
        output =
            std::format("{{\"window\":{{\"slot\":{},\"generation\":{}}},\"target\":",
                        frame.editor.active_window_slot, frame.editor.active_window_generation);
        append_json_string(output, frame.editor.input_focus);
        output.push_back('}');
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
    } else if (path == "render.popup_layout") {
        append_popup_layout(output, frame.render.popup_layout);
    } else if (path == "render.echo_layout") {
        append_echo_layout(output, frame.render.echo_layout);
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
                      .viewport_height = logical_height,
                      .footer_heights =
                          ui::editor_footer_heights(static_cast<float>(frame.render.cell_height))});
    int cell_row = vertical_layout.row_at(logical_y);
    const int overlay_row =
        static_cast<int>(std::floor(logical_y / static_cast<float>(frame.render.cell_height)));
    for (auto iterator = frame.scene.regions.rbegin(); iterator != frame.scene.regions.rend();
         ++iterator) {
        if (iterator->vertical_anchor == ui::VerticalAnchor::Overlay &&
            overlay_row >= iterator->rect.row &&
            overlay_row < iterator->rect.row + iterator->rect.rows &&
            cell_col >= iterator->rect.col && cell_col < iterator->rect.col + iterator->rect.cols) {
            cell_row = overlay_row;
            break;
        }
    }

    std::string output =
        std::format("{{\"window\":{{\"x\":{},\"y\":{}}},\"cell\":{{\"row\":{},\"col\":{}}}",
                    window_x, window_y, cell_row, cell_col);
    if (cell_row >= frame.scene.rows || cell_col >= frame.scene.cols) {
        output += ",\"region\":null,\"prim\":null,\"render\":null}";
        return {true, std::move(output)};
    }

    const ui::Region* hit_region = nullptr;
    std::size_t hit_region_index = frame.scene.regions.size();
    const PrimitiveRenderSnapshot* hit_render = nullptr;
    for (auto iterator = frame.render.primitives.rbegin();
         iterator != frame.render.primitives.rend(); ++iterator) {
        const LogicalPixelRectSnapshot& bounds = iterator->layout_bounds;
        if (logical_x >= bounds.x && logical_x < bounds.x + bounds.width && logical_y >= bounds.y &&
            logical_y < bounds.y + bounds.height &&
            iterator->region_index < frame.scene.regions.size() &&
            iterator->primitive_index < frame.scene.regions[iterator->region_index].prims.size()) {
            hit_render = &*iterator;
            hit_region_index = iterator->region_index;
            hit_region = &frame.scene.regions[hit_region_index];
            break;
        }
    }
    if (!hit_region) {
        for (std::size_t reverse = frame.scene.regions.size(); reverse > 0; --reverse) {
            const std::size_t index = reverse - 1;
            const ui::Region& region = frame.scene.regions[index];
            if (cell_row >= region.rect.row && cell_row < region.rect.row + region.rect.rows &&
                cell_col >= region.rect.col && cell_col < region.rect.col + region.rect.cols) {
                hit_region = &region;
                hit_region_index = index;
                break;
            }
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
    if (hit_render && hit_region->vertical_anchor == ui::VerticalAnchor::Overlay) {
        output += ",\"local_cell\":null";
    } else {
        output += std::format(",\"local_cell\":{{\"row\":{},\"col\":{}}}", local_row, local_col);
    }
    if (hit_region->role == ui::RegionRole::TextArea ||
        hit_region->role == ui::RegionRole::LineNumbers ||
        hit_region->role == ui::RegionRole::ChangeSigns) {
        output += std::format(",\"document_line\":{}",
                              frame.editor.viewport.top_line +
                                  static_cast<std::uint32_t>(std::max(0, local_row)));
    }

    const ui::Prim* hit_prim = nullptr;
    std::size_t hit_primitive_index = hit_region->prims.size();
    if (hit_render) {
        hit_primitive_index = hit_render->primitive_index;
        hit_prim = &hit_region->prims[hit_primitive_index];
    } else {
        for (std::size_t index = 0; index < hit_region->prims.size(); ++index) {
            const ui::Prim& prim = hit_region->prims[index];
            const int width = std::max(1, ui::display_width(prim.text));
            if (prim.row == local_row && local_col >= prim.col && local_col < prim.col + width) {
                hit_prim = &prim;
                hit_primitive_index = index;
            }
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
                hit_render
                    ? hit_render
                    : find_render_primitive(frame.render, hit_region_index, hit_primitive_index)) {
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
    case ui::RegionRole::Popup:
        return "popup";
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
    case ui::StyleClass::Popup:
        return "popup";
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
    output << "    viewport=" << frame.editor.viewport.top_line << '+'
           << frame.editor.viewport.top_line_offset
           << " rows grid-offset=" << frame.scene.grid_offset_rows << '\n';
    output << "    focus=" << printable(frame.editor.input_focus)
           << " window:" << frame.editor.active_window_slot << ':'
           << frame.editor.active_window_generation << '\n';
    output << "    command keymaps=" << frame.editor.command_loop.keymaps.size() << " pending=\""
           << printable(frame.editor.command_loop.pending_keys) << "\" owner=\""
           << printable(frame.editor.command_loop.pending_keymap) << "\" last=\""
           << printable(frame.editor.command_loop.last_command) << "\"\n";
    for (const KeymapLayerStateSnapshot& layer : frame.editor.command_loop.layers) {
        output << "      keymap \"" << printable(layer.name)
               << "\" scope=" << printable(layer.scope);
        if (!layer.parents.empty()) {
            output << " parents=";
            for (std::size_t index = 0; index < layer.parents.size(); ++index) {
                if (index != 0) {
                    output << ',';
                }
                output << printable(layer.parents[index]);
            }
        }
        output << '\n';
    }
    output << "    interaction=" << (frame.editor.interaction.active ? "active" : "inactive")
           << " kind=" << printable(frame.editor.interaction.kind)
           << " provider=" << printable(frame.editor.interaction.provider)
           << " input-cursor=" << frame.editor.interaction.input_cursor
           << " candidates=" << frame.editor.interaction.candidates.size() << '\n';
    output << "    buffers=" << frame.editor.buffers.size() << '\n';
    for (const OpenBufferStateSnapshot& buffer : frame.editor.buffers) {
        output << "      buffer:" << buffer.buffer_slot << ':' << buffer.buffer_generation
               << (buffer.active ? " active" : "") << (buffer.modified ? " modified" : "")
               << " name=\"" << printable(buffer.name) << "\" resource=\""
               << printable(buffer.resource) << "\"\n";
    }
    output << "    windows=" << frame.editor.windows.size() << '\n';
    for (const OpenWindowStateSnapshot& window : frame.editor.windows) {
        output << "      window:" << window.window_slot << ':' << window.window_generation
               << (window.active ? " active" : "") << " view:" << window.view_slot << ':'
               << window.view_generation << " buffer:" << window.buffer_slot << ':'
               << window.buffer_generation << '\n';
    }
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
        if (region.popup) {
            output << "      list first=" << region.popup->first_item
                   << " visible=" << region.popup->items.size()
                   << " total=" << region.popup->total_items << " selected=";
            if (region.popup->selected_item) {
                output << *region.popup->selected_item;
            } else {
                output << "none";
            }
            output << '\n';
        }
        if (region.echo) {
            output << "      echo bytes=" << region.echo->text.size() << " cursor=";
            if (region.echo->cursor_byte) {
                output << *region.echo->cursor_byte;
            } else {
                output << "none";
            }
            output << '\n';
        }
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
    if (frame.render.popup_layout) {
        const PopupLayoutSnapshot& popup = *frame.render.popup_layout;
        output << "    popup-layout panel=(" << popup.panel_bounds.x << ',' << popup.panel_bounds.y
               << ' ' << popup.panel_bounds.width << 'x' << popup.panel_bounds.height
               << ") scroll-x=" << popup.horizontal_scroll << " input=" << popup.input_cursor << '/'
               << popup.input_bytes << " cursor-advance=" << popup.cursor_advance
               << (popup.cursor_clamped ? " clamped" : "") << '\n';
        for (const TextLayoutSnapshot& text : popup.header_text) {
            output << "      text:" << printable(text.role) << " bytes=" << text.byte_count
                   << " advance=" << text.advance << " origin=" << text.origin.x << ':'
                   << text.origin.y << '\n';
        }
    }
    if (frame.render.echo_layout) {
        const EchoLayoutSnapshot& echo = *frame.render.echo_layout;
        output << "    echo-layout bounds=(" << echo.bounds.x << ',' << echo.bounds.y << ' '
               << echo.bounds.width << 'x' << echo.bounds.height
               << ") scroll-x=" << echo.horizontal_scroll << " text=" << echo.text_bytes;
        if (echo.cursor_byte) {
            output << " cursor=" << *echo.cursor_byte << " advance=" << echo.cursor_advance
                   << (echo.cursor_clamped ? " clamped" : "");
        }
        output << '\n';
    }
    output << "    animation=" << (frame.render.animation.active ? "active" : "idle")
           << " scroll=" << (frame.render.animation.scroll ? "true" : "false")
           << " cursor=" << (frame.render.animation.cursor ? "true" : "false")
           << " scroll-progress=" << frame.render.animation.scroll_progress
           << " scroll-velocity=" << frame.render.animation.scroll_velocity
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
            output << "      ! " << printable(primitive.id)
                   << " layout-y=" << primitive.layout_bounds.y << ".."
                   << primitive.layout_bounds.y + primitive.layout_bounds.height;
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
