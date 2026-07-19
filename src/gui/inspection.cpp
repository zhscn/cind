#include "gui/inspection.hpp"

#include "ui/char_width.hpp"
#include "ui/scene_layout.hpp"
#include "ui/view_tree.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
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

void append_setting_value(std::string& output, const SettingValue& value) {
    if (const bool* boolean = std::get_if<bool>(&value)) {
        append_bool(output, *boolean);
    } else if (const std::int64_t* integer = std::get_if<std::int64_t>(&value)) {
        output += std::to_string(*integer);
    } else if (const double* real = std::get_if<double>(&value)) {
        output += std::format("{}", *real);
    } else {
        append_json_string(output, std::get<std::string>(value));
    }
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
    case ui::VerticalAnchor::PaneGrid:
        return "pane-grid";
    case ui::VerticalAnchor::Cell:
        return "cell";
    case ui::VerticalAnchor::Bottom:
        return "bottom";
    case ui::VerticalAnchor::Overlay:
        return "overlay";
    }
    return "unknown";
}

std::string_view modeline_group_name(ModelineGroup group) {
    switch (group) {
    case ModelineGroup::Chip:
        return "chip";
    case ModelineGroup::Left:
        return "left";
    case ModelineGroup::Right:
        return "right";
    }
    return "unknown";
}

std::string_view modeline_tone_name(ModelineTone tone) {
    switch (tone) {
    case ModelineTone::Strong:
        return "strong";
    case ModelineTone::Normal:
        return "normal";
    case ModelineTone::Faded:
        return "faded";
    case ModelineTone::Faint:
        return "faint";
    case ModelineTone::Salient:
        return "salient";
    case ModelineTone::Critical:
        return "critical";
    }
    return "unknown";
}

std::string_view modeline_weight_name(ModelineWeight weight) {
    switch (weight) {
    case ModelineWeight::Regular:
        return "regular";
    case ModelineWeight::Strong:
        return "strong";
    }
    return "unknown";
}

std::string_view view_layer_name(ui::ViewLayer layer) {
    switch (layer) {
    case ui::ViewLayer::Grid:
        return "grid";
    case ui::ViewLayer::Chrome:
        return "chrome";
    case ui::ViewLayer::Overlay:
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
    case ui::PrimKind::PositionHint:
        return "position-hint";
    }
    return "unknown";
}

std::string region_id(const ui::Region& region) {
    return ui::region_view_id(region);
}

std::string_view region_content_name(const ui::Region& region) {
    if (region.popup() != nullptr) {
        return "popup";
    }
    if (region.status() != nullptr) {
        return "status";
    }
    if (region.echo() != nullptr) {
        return "echo";
    }
    return "primitives";
}

std::string primitive_id(const ui::Region& region, const ui::Prim& prim) {
    if (!prim.id.empty()) {
        return prim.id;
    }
    return std::format("{}/cell:{}:{}/{}", region_id(region), prim.row, prim.col,
                       prim_kind_name(prim.kind));
}

std::optional<ui::Prim> inspection_primitive(const ui::Region& region, std::size_t index) {
    if (!region.primitives().empty()) {
        if (index < region.primitives().size()) {
            return region.primitives()[index];
        }
        return std::nullopt;
    }
    if (const ui::Region::PopupContent* popup = region.popup()) {
        const bool band = popup->presentation == ui::Region::PopupPresentation::Band;
        if (band && index == 0) {
            return ui::Prim{0,
                            0,
                            popup->title,
                            ui::StyleClass::StatusKey,
                            false,
                            ui::PrimKind::Text,
                            std::format("{}/title", region_id(region))};
        }
        const std::size_t item_index = index - (band ? 1 : 0);
        if (item_index >= popup->items.size()) {
            return std::nullopt;
        }
        return ui::Prim{
            static_cast<int>(index),
            0,
            popup->items[item_index].label,
            ui::StyleClass::Popup,
            popup->selected_item == popup->first_item + item_index,
            ui::PrimKind::Text,
            std::format("{}/item:{}", region_id(region), popup->first_item + item_index)};
    }
    if (const ModelineContent* status = region.status(); status != nullptr && index == 0) {
        std::string text;
        for (const ModelineSegment& segment : status->segments) {
            if (!text.empty()) {
                text += " ";
            }
            text += segment.text;
        }
        return ui::Prim{0,
                        0,
                        std::move(text),
                        ui::StyleClass::StatusBar,
                        false,
                        ui::PrimKind::Text,
                        std::format("{}/main", region_id(region))};
    }
    if (const ui::Region::EchoContent* echo = region.echo(); echo != nullptr && index == 0) {
        return ui::Prim{0,
                        0,
                        echo->text,
                        ui::StyleClass::Message,
                        false,
                        ui::PrimKind::Text,
                        std::format("{}/main", region_id(region))};
    }
    return std::nullopt;
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
    output += ",\"pending_input_state\":";
    append_json_string(output, command_loop.pending_input_state);
    output += ",\"repeat_count\":";
    if (command_loop.repeat_count) {
        output += std::to_string(*command_loop.repeat_count);
    } else {
        output += "null";
    }
    output += ",\"register\":";
    if (command_loop.register_name) {
        append_json_string(output, *command_loop.register_name);
    } else {
        output += "null";
    }
    output += ",\"prefix_extra\":[";
    for (std::size_t index = 0; index < command_loop.prefix_extra.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"name\":";
        append_json_string(output, command_loop.prefix_extra[index].name);
        output += ",\"value\":";
        append_setting_value(output, command_loop.prefix_extra[index].value);
        output.push_back('}');
    }
    output += "],\"prefix_text\":";
    append_json_string(output, command_loop.prefix_text);
    output += ",\"last_command\":";
    append_json_string(output, command_loop.last_command);
    output.push_back('}');
}

void append_interaction(std::string& output, const InteractionStateSnapshot& interaction) {
    output += "{\"active\":";
    append_bool(output, interaction.active);
    output += std::format(",\"surface\":{{\"window\":{{\"slot\":{},\"generation\":{}}},"
                          "\"buffer\":{{\"slot\":{},\"generation\":{}}},"
                          "\"view\":{{\"slot\":{},\"generation\":{}}}}},"
                          "\"origin\":{{\"window\":{{\"slot\":{},\"generation\":{}}},"
                          "\"buffer\":{{\"slot\":{},\"generation\":{}}},"
                          "\"view\":{{\"slot\":{},\"generation\":{}}}}}",
                          interaction.window_slot, interaction.window_generation,
                          interaction.buffer_slot, interaction.buffer_generation,
                          interaction.view_slot, interaction.view_generation,
                          interaction.origin_window_slot, interaction.origin_window_generation,
                          interaction.origin_buffer_slot, interaction.origin_buffer_generation,
                          interaction.origin_view_slot, interaction.origin_view_generation);
    output += ",\"kind\":";
    append_json_string(output, interaction.kind);
    output += ",\"keymap\":";
    append_json_string(output, interaction.keymap);
    output += ",\"input_state\":";
    append_json_string(output, interaction.input_state);
    output += ",\"buffer_name\":";
    append_json_string(output, interaction.buffer_name);
    output += ",\"prompt\":";
    append_json_string(output, interaction.prompt);
    output += ",\"input\":";
    append_json_string(output, interaction.input);
    output += std::format(",\"input_cursor\":{}", interaction.input_cursor);
    output += ",\"history\":";
    append_json_string(output, interaction.history);
    output += std::format(",\"history_entries\":{}", interaction.history_entries);
    output += ",\"history_index\":";
    if (interaction.history_index) {
        output += std::to_string(*interaction.history_index);
    } else {
        output += "null";
    }
    output += ",\"history_draft\":";
    append_json_string(output, interaction.history_draft);
    output += ",\"provider\":";
    append_json_string(output, interaction.provider);
    output += ",\"allow_custom_input\":";
    append_bool(output, interaction.allow_custom_input);
    output += std::format(",\"generation\":{},\"loading\":", interaction.generation);
    append_bool(output, interaction.loading);
    output += std::format(",\"selected\":{},\"error\":", interaction.selected);
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

void append_completion(std::string& output, const CompletionStateSnapshot& completion) {
    output += "{\"active\":";
    append_bool(output, completion.active);
    output += std::format(",\"generation\":{},\"revision\":{},\"anchor\":{},\"caret\":{},"
                          "\"selected\":{},\"query\":",
                          completion.generation, completion.revision, completion.anchor.value,
                          completion.caret.value, completion.selected);
    append_json_string(output, completion.query);
    output += ",\"pending_providers\":[";
    for (std::size_t index = 0; index < completion.pending_providers.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_json_string(output, completion.pending_providers[index]);
    }
    output += "],\"items\":[";
    for (std::size_t index = 0; index < completion.items.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const CompletionItemStateSnapshot& item = completion.items[index];
        output += std::format("{{\"id\":{},\"provider\":", item.id);
        append_json_string(output, item.provider);
        output += ",\"label\":";
        append_json_string(output, item.label);
        output += ",\"kind\":";
        append_json_string(output, item.kind);
        output += ",\"detail\":";
        append_json_string(output, item.detail);
        output += ",\"resolved\":";
        append_bool(output, item.resolved);
        output += ",\"resolving\":";
        append_bool(output, item.resolving);
        output += ",\"resolve_error\":";
        append_json_string(output, item.resolve_error);
        output += ",\"documentation\":";
        append_json_string(output, item.documentation);
        output.push_back('}');
    }
    output += "]}";
}

void append_lsp(std::string& output, const std::vector<LspSessionStateSnapshot>& sessions) {
    output.push_back('[');
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const LspSessionStateSnapshot& session = sessions[index];
        output += std::format("{{\"id\":{},\"state\":", session.id);
        append_json_string(output, session.state);
        output += ",\"command\":";
        append_json_string(output, session.command);
        output += ",\"root\":";
        append_json_string(output, session.root);
        output += std::format(",\"pending_requests\":{},\"open_documents\":{}",
                              session.pending_requests, session.open_documents);
        output += ",\"server_capabilities\":";
        output += session.server_capabilities;
        output += ",\"error\":";
        append_json_string(output, session.error);
        output.push_back('}');
    }
    output.push_back(']');
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
        output += ",\"major_mode\":";
        append_json_string(output, buffer.major_mode);
        output += ",\"interaction_class\":";
        append_json_string(output, buffer.interaction_class);
        output += ",\"initial_input_state\":";
        append_json_string(output, buffer.initial_input_state);
        output += ",\"completion_auto\":";
        append_bool(output, buffer.completion_auto);
        output += ",\"things\":[";
        for (std::size_t thing_index = 0; thing_index < buffer.things.size(); ++thing_index) {
            if (thing_index != 0) {
                output.push_back(',');
            }
            output += "{\"name\":";
            append_json_string(output, buffer.things[thing_index].name);
            output += ",\"definition\":";
            append_json_string(output, buffer.things[thing_index].definition);
            output.push_back('}');
        }
        output.push_back(']');
        output += ",\"completion_providers\":";
        append_strings(output, buffer.completion_providers);
        output += std::format(",\"location_count\":{},\"diagnostics\":{{\"count\":{},\"errors\":{},"
                              "\"warnings\":{}}}",
                              buffer.location_count, buffer.diagnostic_count,
                              buffer.diagnostic_errors, buffer.diagnostic_warnings);
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
                              "\"buffer\":{{\"slot\":{},\"generation\":{}}},\"role\":",
                              window.window_slot, window.window_generation, window.view_slot,
                              window.view_generation, window.buffer_slot, window.buffer_generation);
        append_json_string(output, window.role);
        output += ",\"pinned\":";
        append_bool(output, window.pinned);
        output += ",\"created_by_policy\":";
        append_bool(output, window.created_by_policy);
        output += ",\"active\":";
        append_bool(output, window.active);
        output += ",\"input_states\":";
        append_strings(output, window.input_states);
        output.push_back('}');
    }
    output.push_back(']');
}

void append_entity(std::string& output, const EntityStateSnapshot& entity) {
    output += std::format("{{\"slot\":{},\"generation\":{}}}", entity.slot, entity.generation);
}

void append_entities(std::string& output, const std::vector<EntityStateSnapshot>& entities) {
    output.push_back('[');
    for (std::size_t index = 0; index < entities.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_entity(output, entities[index]);
    }
    output.push_back(']');
}

void append_workbench_layout(std::string& output, const WorkbenchLayoutStateSnapshot& layout) {
    output += "{\"kind\":";
    append_json_string(output, layout.leaf ? "leaf" : "branch");
    if (layout.leaf) {
        output += ",\"window\":";
        append_entity(output, layout.window);
    } else {
        output += ",\"axis\":";
        append_json_string(output, layout.axis);
        output += std::format(",\"ratio\":{},\"children\":[", layout.ratio);
        for (std::size_t index = 0; index < layout.children.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            append_workbench_layout(output, layout.children[index]);
        }
        output.push_back(']');
    }
    output.push_back('}');
}

void append_workbenches(std::string& output,
                        const std::vector<WorkbenchStateSnapshot>& workbenches) {
    output.push_back('[');
    for (std::size_t index = 0; index < workbenches.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const WorkbenchStateSnapshot& workbench = workbenches[index];
        output += "{\"workbench\":";
        append_entity(output, workbench.workbench);
        output += ",\"name\":";
        append_json_string(output, workbench.name);
        output += ",\"active\":";
        append_bool(output, workbench.active);
        output += ",\"scope\":";
        append_entities(output, workbench.scope);
        output += ",\"mru\":";
        append_entities(output, workbench.mru);
        output += ",\"active_window\":";
        append_entity(output, workbench.active_window);
        output += ",\"slots\":[";
        for (std::size_t slot = 0; slot < workbench.slots.size(); ++slot) {
            if (slot != 0) {
                output.push_back(',');
            }
            output += "{\"role\":";
            append_json_string(output, workbench.slots[slot].role);
            output += ",\"window\":";
            append_entity(output, workbench.slots[slot].window);
            output.push_back('}');
        }
        output += "],\"layout\":";
        append_workbench_layout(output, workbench.layout);
        output += ",\"windows\":";
        append_windows(output, workbench.windows);
        output.push_back('}');
    }
    output.push_back(']');
}

void append_projects(std::string& output, const std::vector<ProjectStateSnapshot>& projects) {
    output.push_back('[');
    for (std::size_t index = 0; index < projects.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const ProjectStateSnapshot& project = projects[index];
        output += std::format("{{\"project\":{{\"slot\":{},\"generation\":{}}},\"name\":",
                              project.project_slot, project.project_generation);
        append_json_string(output, project.name);
        output += ",\"roots\":";
        append_strings(output, project.roots);
        output += ",\"discovery_provider\":";
        append_json_string(output, project.discovery_provider);
        output += ",\"discovery_marker\":";
        append_json_string(output, project.discovery_marker);
        output += std::format(",\"file_count\":{},\"index_revision\":{},\"indexing\":",
                              project.file_count, project.index_revision);
        append_bool(output, project.indexing);
        output += ",\"index_error\":";
        append_json_string(output, project.index_error);
        output.push_back('}');
    }
    output.push_back(']');
}

void append_location(std::string& output, const LocationStateSnapshot& location) {
    if (!location.present) {
        output += "null";
        return;
    }
    output += std::format("{{\"source_range\":{{\"start\":{},\"end\":{}}},\"resource\":",
                          location.source_range.start.value, location.source_range.end.value);
    append_json_string(output, location.resource);
    output +=
        std::format(",\"target\":{{\"line\":{},\"column\":{},\"encoding\":", location.target.line,
                    location.target.column);
    append_json_string(output,
                       location.target.encoding == PositionEncoding::Utf16 ? "utf-16" : "bytes");
    output += "}}";
}

void append_location_navigation(std::string& output,
                                const LocationNavigationStateSnapshot& navigation) {
    if (!navigation.present) {
        output += "null";
        return;
    }
    output += std::format("{{\"buffer\":{{\"slot\":{},\"generation\":{}}},\"selected_index\":",
                          navigation.buffer_slot, navigation.buffer_generation);
    if (navigation.selected_index) {
        output += std::to_string(*navigation.selected_index);
    } else {
        output += "null";
    }
    output += std::format(",\"location_count\":{}}}", navigation.location_count);
}

void append_jumps(std::string& output, const std::vector<WorkbenchJumpStateSnapshot>& graphs) {
    output.push_back('[');
    for (std::size_t graph_index = 0; graph_index < graphs.size(); ++graph_index) {
        if (graph_index != 0) {
            output.push_back(',');
        }
        const WorkbenchJumpStateSnapshot& graph = graphs[graph_index];
        output += "{\"workbench\":";
        append_entity(output, graph.workbench);
        output += ",\"nodes\":[";
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const JumpNodeStateSnapshot& node = graph.nodes[index];
            output += std::format("{{\"id\":{},\"attached\":", node.id);
            append_bool(output, node.attached);
            output += ",\"buffer\":";
            append_entity(output, node.buffer);
            output += std::format(",\"anchor\":{},\"resource\":", node.anchor);
            append_json_string(output, node.resource);
            output += std::format(",\"fallback\":{{\"line\":{},\"byte_column\":{}}},\"excerpt\":",
                                  node.fallback.line, node.fallback.byte_column);
            append_json_string(output, node.excerpt);
            output += std::format(",\"created_at\":{},\"last_visit\":{}}}", node.created_at,
                                  node.last_visit);
        }
        output += "],\"edges\":[";
        for (std::size_t index = 0; index < graph.edges.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const JumpEdgeStateSnapshot& edge = graph.edges[index];
            output += std::format("{{\"from\":{},\"to\":{},\"kind\":", edge.from, edge.to);
            append_json_string(output, edge.kind);
            output += std::format(",\"at\":{},\"persistent\":", edge.at);
            append_bool(output, edge.persistent);
            output.push_back('}');
        }
        output += "],\"walks\":[";
        for (std::size_t index = 0; index < graph.walks.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const JumpWalkStateSnapshot& walk = graph.walks[index];
            output += "{\"window\":";
            append_entity(output, walk.window);
            output += ",\"entries\":[";
            for (std::size_t entry = 0; entry < walk.entries.size(); ++entry) {
                if (entry != 0) {
                    output.push_back(',');
                }
                output += std::to_string(walk.entries[entry]);
            }
            output += "],\"cursor\":";
            output += walk.cursor ? std::to_string(*walk.cursor) : "null";
            output.push_back('}');
        }
        output += "]}";
    }
    output.push_back(']');
}

void append_scripting(std::string& output, const ScriptingStateSnapshot& scripting) {
    output += "{\"engine\":";
    append_json_string(output, scripting.engine);
    output += ",\"version\":";
    append_json_string(output, scripting.version);
    output += ",\"modules\":";
    append_strings(output, scripting.modules);
    output += ",\"extensions\":";
    append_strings(output, scripting.extensions);
    output += std::format(
        ",\"command_revision\":{},\"scripted_commands\":{},\"provider_revision\":{},"
        "\"scripted_providers\":{},\"binding_revision\":{},\"input_state_revision\":{},"
        "\"scripted_input_states\":{},\"scripted_input_strategies\":{},"
        "\"mode_revision\":{},\"scripted_modes\":{},"
        "\"resource_policy_revision\":{},\"scripted_file_mode_rules\":{},"
        "\"scripted_project_providers\":{},\"outstanding_async_tasks\":{},\"last_error\":",
        scripting.command_revision, scripting.scripted_commands, scripting.provider_revision,
        scripting.scripted_providers, scripting.binding_revision, scripting.input_state_revision,
        scripting.scripted_input_states, scripting.scripted_input_strategies,
        scripting.mode_revision, scripting.scripted_modes, scripting.resource_policy_revision,
        scripting.scripted_file_mode_rules, scripting.scripted_project_providers,
        scripting.outstanding_async_tasks);
    if (scripting.last_error) {
        append_json_string(output, *scripting.last_error);
    } else {
        output += "null";
    }
    output.push_back('}');
}

void append_selection(std::string& output, const SelectionStateSnapshot& selection) {
    output += "{\"active\":";
    append_bool(output, selection.active);
    output += std::format(",\"primary\":{},\"history_depth\":{},\"metadata\":", selection.primary,
                          selection.history_depth);
    append_json_string(output, selection.metadata);
    output += ",\"ranges\":[";
    for (std::size_t index = 0; index < selection.ranges.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const SelectionRangeStateSnapshot& range = selection.ranges[index];
        output += std::format("{{\"anchor\":{},\"head\":{},\"granularity\":", range.anchor.value,
                              range.head.value);
        append_json_string(output, range.granularity);
        output.push_back('}');
    }
    output += "]}";
}

void append_position_hints(std::string& output, const PositionHintsStateSnapshot& hints) {
    output += "{\"provider\":";
    append_bool(output, hints.provider);
    output += ",\"items\":[";
    for (std::size_t index = 0; index < hints.items.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const PositionHintStateSnapshot& hint = hints.items[index];
        output += std::format("{{\"byte\":{},\"label\":", hint.position.value);
        append_json_string(output, hint.label);
        output.push_back('}');
    }
    output += "],\"error\":";
    if (hints.error) {
        append_json_string(output, *hints.error);
    } else {
        output += "null";
    }
    output.push_back('}');
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
    output += ",\"selection\":";
    append_selection(output, editor.selection);
    output += ",\"position_hints\":";
    append_position_hints(output, editor.position_hints);
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
    output += ",\"input_strategy\":";
    append_json_string(output, editor.input_strategy);
    output += ",\"input_state\":";
    append_json_string(output, editor.input_state);
    output += ",\"input_cursor_shape\":";
    append_json_string(output, editor.input_cursor_shape);
    output += ",\"input_state_indicator\":";
    append_json_string(output, editor.input_state_indicator);
    output += ",\"text_input_policy\":";
    append_json_string(output, editor.text_input_policy);
    output += ",\"text_input_command\":";
    append_json_string(output, editor.text_input_command);
    output += ",\"text_input_command_available\":";
    append_bool(output, editor.text_input_command_available);
    output += ",\"selection_after_edit\":";
    append_json_string(output, editor.selection_after_edit);
    output += ",\"input_state_handler\":";
    append_bool(output, editor.input_state_handler);
    output += ",\"input_state_on_enter\":";
    append_bool(output, editor.input_state_on_enter);
    output += ",\"input_state_on_exit\":";
    append_bool(output, editor.input_state_on_exit);
    output += ",\"command_loop\":";
    append_command_loop(output, editor.command_loop);
    output += ",\"scripting\":";
    append_scripting(output, editor.scripting);
    output += ",\"interaction\":";
    append_interaction(output, editor.interaction);
    output += ",\"completion\":";
    append_completion(output, editor.completion);
    output += ",\"lsp\":";
    append_lsp(output, editor.lsp);
    output += ",\"buffers\":";
    append_buffers(output, editor.buffers);
    output += ",\"windows\":";
    append_windows(output, editor.windows);
    output += ",\"workbenches\":";
    append_workbenches(output, editor.workbenches);
    output += ",\"projects\":";
    append_projects(output, editor.projects);
    output += ",\"location_at_caret\":";
    append_location(output, editor.location_at_caret);
    output += ",\"location_navigation\":";
    append_location_navigation(output, editor.location_navigation);
    output += ",\"jumps\":";
    append_jumps(output, editor.jumps);
    output += ",\"background_work\":";
    append_bool(output, editor.background_work);
    output += ",\"project_search_running\":";
    append_bool(output, editor.project_search_running);
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
    output += std::format(",\"span_cols\":{}", prim.span_cols);
    output.push_back('}');
}

void append_hit_target(std::string& output, const ui::HitTarget& target) {
    output += "{\"kind\":";
    append_json_string(output, ui::hit_target_kind_name(target.kind));
    output += ",\"view_id\":";
    append_json_string(output, target.view_id);
    output += ",\"pane_id\":";
    append_json_string(output, target.pane_id);
    output += std::format(",\"region_index\":{},\"role\":", target.region_index);
    append_json_string(output, region_role_name(target.role));
    output += ",\"document_line\":";
    if (target.document_line) {
        output += std::to_string(*target.document_line);
    } else {
        output += "null";
    }
    output += ",\"display_column\":";
    if (target.display_column) {
        output += std::to_string(*target.display_column);
    } else {
        output += "null";
    }
    output += ",\"popup_item\":";
    if (target.popup_item) {
        output += std::to_string(*target.popup_item);
    } else {
        output += "null";
    }
    output.push_back('}');
}

void append_region(std::string& output, const ui::Region& region) {
    output += "{\"id\":";
    append_json_string(output, region_id(region));
    output += ",\"role\":";
    append_json_string(output, region_role_name(region.role));
    output += ",\"surface\":";
    append_json_string(output, surface_class_name(region.surface));
    output += ",\"vertical_anchor\":";
    append_json_string(output, vertical_anchor_name(region.vertical_anchor));
    output += ",\"pane_id\":";
    append_json_string(output, region.pane_id);
    output += ",\"active\":";
    append_bool(output, region.active);
    output += std::format(",\"content_offset_rows\":{},\"revision\":{}", region.content_offset_rows,
                          region.revision);
    output += ",\"content_type\":";
    append_json_string(output, region_content_name(region));
    output += ",\"document_mapping\":";
    if (const ui::Region::DocumentMapping* mapping = region.document_mapping()) {
        output += std::format("{{\"first_line\":{},\"first_display_column\":", mapping->first_line);
        if (mapping->first_display_column) {
            output += std::to_string(*mapping->first_display_column);
        } else {
            output += "null";
        }
        output.push_back('}');
    } else {
        output += "null";
    }
    output += ",\"rect\":";
    append_rect(output, region.rect);
    output += ",\"popup\":";
    if (const ui::Region::PopupContent* popup = region.popup()) {
        output += "{\"presentation\":";
        append_json_string(output, popup->presentation == ui::Region::PopupPresentation::Completion
                                       ? "completion"
                                       : "band");
        output += ",\"title\":";
        append_json_string(output, popup->title);
        output += ",\"input\":";
        if (popup->input) {
            append_json_string(output, *popup->input);
        } else {
            output += "null";
        }
        output += ",\"input_cursor\":";
        if (popup->input_cursor) {
            output += std::to_string(*popup->input_cursor);
        } else {
            output += "null";
        }
        output += std::format(",\"first_item\":{},\"total_items\":{}", popup->first_item,
                              popup->total_items);
        output += ",\"selected_item\":";
        if (popup->selected_item) {
            output += std::to_string(*popup->selected_item);
        } else {
            output += "null";
        }
        output += ",\"items\":[";
        for (std::size_t index = 0; index < popup->items.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            output += "{\"label\":";
            append_json_string(output, popup->items[index].label);
            output += ",\"detail\":";
            append_json_string(output, popup->items[index].detail);
            output += ",\"kind\":";
            append_json_string(output, popup->items[index].kind);
            output.push_back('}');
        }
        output += "]}";
    } else {
        output += "null";
    }
    output += ",\"status\":";
    if (const ModelineContent* status = region.status()) {
        output += "{\"segments\":[";
        for (std::size_t index = 0; index < status->segments.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const ModelineSegment& segment = status->segments[index];
            output += "{\"text\":";
            append_json_string(output, segment.text);
            output += ",\"group\":";
            append_json_string(output, modeline_group_name(segment.group));
            output += ",\"tone\":";
            append_json_string(output, modeline_tone_name(segment.tone));
            output += ",\"weight\":";
            append_json_string(output, modeline_weight_name(segment.weight));
            output += ",\"debug\":";
            append_bool(output, segment.debug);
            output.push_back('}');
        }
        output += "]}";
    } else {
        output += "null";
    }
    output += ",\"echo\":";
    if (const ui::Region::EchoContent* echo = region.echo()) {
        output += "{\"text\":";
        append_json_string(output, echo->text);
        output += ",\"cursor_byte\":";
        if (echo->cursor_byte) {
            output += std::to_string(*echo->cursor_byte);
        } else {
            output += "null";
        }
        output += ",\"key\":";
        append_json_string(output, echo->key);
        output.push_back('}');
    } else {
        output += "null";
    }
    output += ",\"prims\":[";
    for (std::size_t index = 0; index < region.primitives().size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_prim(output, region, region.primitives()[index]);
    }
    output += "]}";
}

void append_view_tree(std::string& output, const ui::Scene& scene) {
    const ui::ViewTree tree(scene);
    output += "{\"id\":\"scene\",\"layers\":[";
    for (std::size_t layer_index = 0; layer_index < tree.layers().size(); ++layer_index) {
        if (layer_index != 0) {
            output.push_back(',');
        }
        const ui::ViewLayerNode& layer = tree.layers()[layer_index];
        output += "{\"id\":";
        append_json_string(output, layer.id);
        output += ",\"layer\":";
        append_json_string(output, view_layer_name(layer.layer));
        output += ",\"children\":[";
        for (std::size_t index = 0; index < layer.children.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const ui::ViewNode& node = layer.children[index];
            output += "{\"id\":";
            append_json_string(output, node.id);
            output += std::format(",\"region_index\":{},\"role\":", node.region_index);
            append_json_string(output, region_role_name(node.role));
            output += ",\"rect\":";
            append_rect(output, node.rect);
            output.push_back('}');
        }
        output += "]}";
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
    output += ",\"shape\":";
    append_json_string(output, cursor_shape_name(scene.cursor_shape));
    output += "},\"view_tree\":";
    append_view_tree(output, scene);
    output += ",\"panes\":[";
    for (std::size_t index = 0; index < scene.panes.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const ui::ScenePane& pane = scene.panes[index];
        output += "{\"id\":";
        append_json_string(output, pane.id);
        output += ",\"rect\":";
        append_rect(output, pane.rect);
        output += ",\"active\":";
        append_bool(output, pane.active);
        output.push_back('}');
    }
    output += "],\"dividers\":[";
    for (std::size_t index = 0; index < scene.dividers.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const ui::SceneDivider& divider = scene.dividers[index];
        output += "{\"id\":";
        append_json_string(output, divider.id);
        output += ",\"axis\":";
        append_json_string(output,
                           divider.axis == ui::DividerAxis::Horizontal ? "horizontal" : "vertical");
        output += std::format(",\"position\":{},\"start\":{},\"length\":{}}}", divider.position,
                              divider.start, divider.length);
    }
    output += "],\"regions\":[";
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

void append_presentation_styles(std::string& output, const PresentationStyleSheet& styles) {
    output += std::format("{{\"inactive_alpha\":{},\"secondary_alpha\":{},\"text\":[",
                          styles.inactive_alpha, styles.secondary_alpha);
    for (std::size_t index = 0; index < styles.text.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const PresentationTextStyle& style = styles.text[index];
        output += "{\"role\":";
        append_json_string(output,
                           presentation_text_role_name(static_cast<PresentationTextRole>(index)));
        output += ",\"foreground\":";
        append_color(output, style.foreground);
        output += ",\"background\":";
        if (style.background) {
            append_color(output, *style.background);
        } else {
            output += "null";
        }
        output += ",\"weight\":";
        append_json_string(output,
                           style.weight == PresentationWeight::Strong ? "strong" : "regular");
        output.push_back('}');
    }
    output += "],\"modeline\":[";
    for (std::size_t index = 0; index < styles.modeline.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_color(output, styles.modeline[index]);
    }
    output += "]}";
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

void append_document_layout(std::string& output,
                            const std::optional<DocumentLayoutSnapshot>& document) {
    if (!document) {
        output += "null";
        return;
    }
    output += "{\"coordinate_space\":\"logical-pixels\",\"bounds\":";
    append_logical_rect(output, document->bounds);
    output += ",\"cursor_row\":";
    if (document->cursor_row) {
        output += std::to_string(*document->cursor_row);
    } else {
        output += "null";
    }
    output += ",\"cursor_column\":";
    if (document->cursor_column) {
        output += std::to_string(*document->cursor_column);
    } else {
        output += "null";
    }
    output += std::format(",\"cursor_advance\":{},\"grid_cursor_x\":{},\"cursor_rect\":",
                          document->cursor_advance, document->grid_cursor_x);
    if (document->cursor_rect) {
        append_logical_rect(output, *document->cursor_rect);
    } else {
        output += "null";
    }
    output += ",\"lines\":[";
    for (std::size_t index = 0; index < document->lines.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const DocumentLineLayoutSnapshot& line = document->lines[index];
        output +=
            std::format("{{\"row\":{},\"end_column\":{},\"origin_x\":{},"
                        "\"advance\":{},\"run_count\":{}}}",
                        line.row, line.end_column, line.origin_x, line.advance, line.run_count);
    }
    output += "]}";
}

void append_render_damage(std::string& output, const RenderDamageSnapshot& damage) {
    output += "{\"full_repaint\":";
    append_bool(output, damage.full_repaint);
    output += ",\"grid_transform_changed\":";
    append_bool(output, damage.grid_transform_changed);
    output += std::format(",\"grid_translation_rows\":{},\"damaged_cells\":{},"
                          "\"damaged_output_pixels\":{},"
                          "\"output_fraction\":{},\"full_reference_match\":",
                          damage.grid_translation_rows, damage.damaged_cells,
                          damage.damaged_output_pixels, damage.output_fraction);
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
    output += ",\"cursor_constrained\":";
    append_bool(output, animation.cursor_constrained);
    output += ",\"cursor_owner\":";
    append_json_string(output, animation.cursor_owner);
    output +=
        std::format(",\"scroll_progress\":{},\"cursor_progress\":{},\"scroll_velocity\":{},"
                    "\"visual_scroll_top\":{},\"target_scroll_top\":{},\"layers\":[",
                    animation.scroll_progress, animation.cursor_progress, animation.scroll_velocity,
                    animation.visual_scroll_top, animation.target_scroll_top);
    for (std::size_t index = 0; index < animation.layers.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += std::format(
            "{{\"scroll_top\":{},\"grid_offset_y\":{},\"clip_top\":{},\"clip_bottom\":{}}}",
            animation.layers[index].scroll_top, animation.layers[index].grid_offset_y,
            animation.layers[index].clip_top, animation.layers[index].clip_bottom);
    }
    output += "],\"active_line_y\":";
    if (animation.active_line_y) {
        output += std::format("{}", *animation.active_line_y);
    } else {
        output += "null";
    }
    output += ",\"cursor_rect\":";
    if (animation.cursor_rect) {
        append_logical_rect(output, *animation.cursor_rect);
    } else {
        output += "null";
    }
    output.push_back('}');
}

void append_render_timings(std::string& output, const RenderTimingSnapshot& timings) {
    output += std::format(
        "{{\"layout_us\":{},\"compose_us\":{},\"render_state_us\":{},\"inspect_us\":{},"
        "\"frame_build_us\":{},\"raster_us\":{},\"reference_us\":{},\"upload_us\":{},"
        "\"present_us\":{},\"total_us\":{},\"uploaded_bytes\":{},\"upload_rects\":{},"
        "\"texture_scroll_reused\":{},\"texture_copy_pixels\":{},"
        "\"shape_cache_hits\":{},\"shape_cache_misses\":{},"
        "\"shape_cache_evictions\":{},\"shape_cache_entries\":{}}}",
        timings.layout_us, timings.compose_us, timings.render_state_us, timings.inspect_us,
        timings.frame_build_us, timings.raster_us, timings.reference_us, timings.upload_us,
        timings.present_us, timings.total_us, timings.uploaded_bytes, timings.upload_rects,
        timings.texture_scroll_reused, timings.texture_copy_pixels, timings.shape_cache_hits,
        timings.shape_cache_misses, timings.shape_cache_evictions, timings.shape_cache_entries);
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
    output += ",\"highlight\":";
    append_color(output, render.theme.highlight);
    output += ",\"band\":";
    append_color(output, render.theme.band);
    output += ",\"selection\":";
    append_color(output, render.theme.selection);
    output += ",\"divider\":";
    append_color(output, render.theme.divider);
    output += ",\"text\":";
    append_color(output, render.theme.text);
    output += ",\"strong\":";
    append_color(output, render.theme.strong);
    output += ",\"faded\":";
    append_color(output, render.theme.faded);
    output += ",\"faint\":";
    append_color(output, render.theme.faint);
    output += ",\"salient\":";
    append_color(output, render.theme.salient);
    output += ",\"popout\":";
    append_color(output, render.theme.popout);
    output += ",\"critical\":";
    append_color(output, render.theme.critical);
    output += ",\"cursor\":";
    append_color(output, render.theme.cursor);
    output += ",\"sign_added\":";
    append_color(output, render.theme.sign_added);
    output += ",\"sign_modified\":";
    append_color(output, render.theme.sign_modified);
    output += ",\"sign_deleted\":";
    append_color(output, render.theme.sign_deleted);
    output += "},\"styles\":";
    append_presentation_styles(output, render.styles);
    output +=
        std::format(",\"metrics\":{{\"modeline_extra_height\":{},\"echo_extra_height\":{},"
                    "\"footer_padding_x\":{},\"segment_gap\":{},\"chip_padding_x\":{},"
                    "\"minibuffer_padding_x\":{},\"minibuffer_detail_gap\":{},\"cursor_stroke\":{},"
                    "\"minimum_columns\":{},\"minimum_rows\":{}}},\"pixel_hash\":\"0x{:016X}\","
                    "\"animation\":",
                    render.metrics.modeline_extra_height, render.metrics.echo_extra_height,
                    render.metrics.footer_padding_x, render.metrics.segment_gap,
                    render.metrics.chip_padding_x, render.metrics.minibuffer_padding_x,
                    render.metrics.minibuffer_detail_gap, render.metrics.cursor_stroke,
                    render.metrics.minimum_columns, render.metrics.minimum_rows, render.pixel_hash);
    append_render_animation(output, render.animation);
    output += ",\"damage\":";
    append_render_damage(output, render.damage);
    output += ",\"timings\":";
    append_render_timings(output, render.timings);
    output += ",\"document_layout\":";
    append_document_layout(output, render.document_layout);
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
        primitive.region = region_id(region);
        const std::optional<ui::Prim> scene_primitive =
            inspection_primitive(region, primitive.primitive_index);
        if (!scene_primitive) {
            continue;
        }
        primitive.id = primitive_id(region, *scene_primitive);
        primitive.kind = std::string(prim_kind_name(scene_primitive->kind));
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
    if (frame.editor.selection.ranges.empty() ||
        frame.editor.selection.primary >= frame.editor.selection.ranges.size()) {
        violations.emplace_back("editor selection has no valid primary range");
    }
    for (const SelectionRangeStateSnapshot& range : frame.editor.selection.ranges) {
        if (range.anchor.value > frame.editor.document_bytes ||
            range.head.value > frame.editor.document_bytes) {
            violations.emplace_back("editor selection range is past the document end");
            break;
        }
    }
    if (!frame.editor.position_hints.provider &&
        (!frame.editor.position_hints.items.empty() || frame.editor.position_hints.error)) {
        violations.emplace_back("editor position hints exist without an active provider");
    }
    for (const PositionHintStateSnapshot& hint : frame.editor.position_hints.items) {
        if (hint.position.value > frame.editor.document_bytes) {
            violations.emplace_back("editor position hint is past the document end");
            break;
        }
        if (hint.label.empty()) {
            violations.emplace_back("editor position hint label is empty");
            break;
        }
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
    const std::size_t active_workbenches = static_cast<std::size_t>(std::ranges::count_if(
        frame.editor.workbenches,
        [](const WorkbenchStateSnapshot& workbench) { return workbench.active; }));
    if (frame.editor.workbenches.empty() || active_workbenches != 1) {
        violations.emplace_back("editor must have exactly one active workbench");
    }
    const auto has_duplicate_entity = [](const std::vector<EntityStateSnapshot>& entities) {
        for (std::size_t index = 0; index < entities.size(); ++index) {
            if (std::ranges::find(entities.begin() + static_cast<std::ptrdiff_t>(index + 1),
                                  entities.end(), entities[index]) != entities.end()) {
                return true;
            }
        }
        return false;
    };
    std::vector<EntityStateSnapshot> owned_windows;
    for (const WorkbenchStateSnapshot& workbench : frame.editor.workbenches) {
        const std::size_t focused = static_cast<std::size_t>(
            std::ranges::count_if(workbench.windows, [](const OpenWindowStateSnapshot& window) {
                return window.active;
            }));
        if (workbench.windows.empty() || focused != 1) {
            violations.emplace_back("workbench must have exactly one active window");
        } else {
            const auto selected =
                std::ranges::find_if(workbench.windows, [](const OpenWindowStateSnapshot& window) {
                    return window.active;
                });
            if (selected->window_slot != workbench.active_window.slot ||
                selected->window_generation != workbench.active_window.generation) {
                violations.emplace_back(
                    "workbench active window identity does not match window state");
            }
        }
        if (workbench.active &&
            (workbench.active_window.slot != frame.editor.active_window_slot ||
             workbench.active_window.generation != frame.editor.active_window_generation)) {
            violations.emplace_back("active workbench does not own the editor active window");
        }
        for (const OpenWindowStateSnapshot& window : workbench.windows) {
            const EntityStateSnapshot identity{.slot = window.window_slot,
                                               .generation = window.window_generation};
            if (std::ranges::find(owned_windows, identity) != owned_windows.end()) {
                violations.emplace_back("window belongs to more than one workbench");
            } else {
                owned_windows.push_back(identity);
            }
        }
        if (has_duplicate_entity(workbench.scope)) {
            violations.emplace_back("workbench scope contains duplicate projects");
        }
        if (std::ranges::any_of(workbench.scope, [&](const EntityStateSnapshot& project) {
                return std::ranges::none_of(
                    frame.editor.projects, [&](const ProjectStateSnapshot& candidate) {
                        return candidate.project_slot == project.slot &&
                               candidate.project_generation == project.generation;
                    });
            })) {
            violations.emplace_back("workbench scope contains a stale project");
        }
        if (has_duplicate_entity(workbench.mru)) {
            violations.emplace_back("workbench MRU contains duplicate buffers");
        }
        if (std::ranges::any_of(workbench.mru, [&](const EntityStateSnapshot& buffer) {
                return std::ranges::none_of(
                    frame.editor.buffers, [&](const OpenBufferStateSnapshot& candidate) {
                        return candidate.buffer_slot == buffer.slot &&
                               candidate.buffer_generation == buffer.generation;
                    });
            })) {
            violations.emplace_back("workbench MRU contains a stale buffer");
        }
        std::vector<EntityStateSnapshot> layout_windows;
        const auto validate_layout = [&](this const auto& self,
                                         const WorkbenchLayoutStateSnapshot& node) -> void {
            if (node.leaf) {
                if (!node.children.empty()) {
                    violations.emplace_back("workbench layout leaf has children");
                }
                layout_windows.push_back(node.window);
                return;
            }
            if (node.children.size() != 2 || (node.axis != "rows" && node.axis != "columns") ||
                !std::isfinite(node.ratio) || node.ratio <= 0.0F || node.ratio >= 1.0F) {
                violations.emplace_back("workbench layout branch is invalid");
                return;
            }
            self(node.children[0]);
            self(node.children[1]);
        };
        validate_layout(workbench.layout);
        if (layout_windows.size() != workbench.windows.size() ||
            std::ranges::any_of(workbench.windows, [&](const OpenWindowStateSnapshot& window) {
                return std::ranges::find(
                           layout_windows,
                           EntityStateSnapshot{.slot = window.window_slot,
                                               .generation = window.window_generation}) ==
                       layout_windows.end();
            })) {
            violations.emplace_back("workbench layout leaves do not match owned windows");
        }
        for (const WorkbenchSlotStateSnapshot& slot : workbench.slots) {
            const auto window = std::ranges::find_if(
                workbench.windows, [&](const OpenWindowStateSnapshot& candidate) {
                    return candidate.window_slot == slot.window.slot &&
                           candidate.window_generation == slot.window.generation;
                });
            if (slot.role.empty() || window == workbench.windows.end() ||
                window->role != slot.role) {
                violations.emplace_back("workbench slot does not match a role-bearing window");
            }
        }
        for (std::size_t index = 0; index < workbench.slots.size(); ++index) {
            if (std::ranges::any_of(
                    workbench.slots.begin() + static_cast<std::ptrdiff_t>(index + 1),
                    workbench.slots.end(), [&](const WorkbenchSlotStateSnapshot& slot) {
                        return slot.role == workbench.slots[index].role;
                    })) {
                violations.emplace_back("workbench contains duplicate slot roles");
                break;
            }
        }
    }
    if (frame.editor.jumps.size() != frame.editor.workbenches.size()) {
        violations.emplace_back("jump graph count does not match workbenches");
    }
    for (const WorkbenchJumpStateSnapshot& graph : frame.editor.jumps) {
        const auto owner = std::ranges::find(frame.editor.workbenches, graph.workbench,
                                             &WorkbenchStateSnapshot::workbench);
        if (owner == frame.editor.workbenches.end()) {
            violations.emplace_back("jump graph has no owning workbench");
            continue;
        }
        std::vector<std::uint64_t> node_ids;
        node_ids.reserve(graph.nodes.size());
        for (const JumpNodeStateSnapshot& node : graph.nodes) {
            if (node.id == 0 || std::ranges::find(node_ids, node.id) != node_ids.end()) {
                violations.emplace_back("jump graph contains an invalid or duplicate node");
                break;
            }
            node_ids.push_back(node.id);
            if (node.attached != (node.buffer.slot != 0 || node.buffer.generation != 0) ||
                node.attached != (node.anchor != 0)) {
                violations.emplace_back("jump node attachment state is inconsistent");
                break;
            }
        }
        for (const JumpEdgeStateSnapshot& edge : graph.edges) {
            if (edge.kind.empty() || std::ranges::find(node_ids, edge.from) == node_ids.end() ||
                std::ranges::find(node_ids, edge.to) == node_ids.end()) {
                violations.emplace_back("jump edge references an unavailable node");
                break;
            }
        }
        if (graph.walks.size() != owner->windows.size()) {
            violations.emplace_back("jump walks do not match workbench windows");
        }
        for (const JumpWalkStateSnapshot& walk : graph.walks) {
            const auto window =
                std::ranges::find_if(owner->windows, [&](const OpenWindowStateSnapshot& candidate) {
                    return candidate.window_slot == walk.window.slot &&
                           candidate.window_generation == walk.window.generation;
                });
            if (window == owner->windows.end() ||
                (walk.entries.empty() != !walk.cursor.has_value()) ||
                (walk.cursor && *walk.cursor >= walk.entries.size()) ||
                std::ranges::any_of(walk.entries, [&](std::uint64_t node) {
                    return std::ranges::find(node_ids, node) == node_ids.end();
                })) {
                violations.emplace_back("jump walk state is inconsistent");
                break;
            }
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
                                                ? std::string_view("minibuffer")
                                                : std::string_view("window");
    if (frame.editor.input_focus != expected_focus) {
        violations.emplace_back("editor input focus does not match interaction state");
    }
    if (frame.editor.text_input_policy == "accept" && !frame.editor.text_input_command_available) {
        violations.emplace_back("accepting input state has no available text command");
    }
    const std::string_view expected_cursor_shape = frame.editor.input_cursor_shape;
    if (cursor_shape_name(frame.scene.cursor_shape) != expected_cursor_shape) {
        violations.emplace_back("scene cursor shape does not match active input policy");
    }
    if (frame.editor.interaction.input_cursor > frame.editor.interaction.input.size()) {
        violations.emplace_back("interaction input cursor is past the input end");
    }
    if (frame.editor.interaction.history_index &&
        *frame.editor.interaction.history_index >= frame.editor.interaction.history_entries) {
        violations.emplace_back("interaction history cursor is past the history end");
    }
    if (frame.editor.command_loop.keymaps.size() != frame.editor.command_loop.layers.size() ||
        !std::ranges::equal(frame.editor.command_loop.keymaps, frame.editor.command_loop.layers,
                            [](const std::string& keymap, const KeymapLayerStateSnapshot& layer) {
                                return keymap == layer.name;
                            })) {
        violations.emplace_back("command keymap names do not match layer state");
    }
    if (frame.editor.command_loop.layers.empty() ||
        (expected_focus == "minibuffer") !=
            (frame.editor.command_loop.layers.front().scope == "minibuffer")) {
        violations.emplace_back("command keymap layers do not match input focus");
    }
    if (frame.editor.interaction.active &&
        (frame.editor.interaction.origin_window_slot != frame.editor.active_window_slot ||
         frame.editor.interaction.origin_window_generation !=
             frame.editor.active_window_generation)) {
        violations.emplace_back("minibuffer origin does not match the active document window");
    }
    if (frame.editor.interaction.active &&
        !std::ranges::contains(frame.editor.command_loop.keymaps,
                               frame.editor.interaction.keymap)) {
        violations.emplace_back("interaction keymap is absent from the command layer stack");
    }
    if (frame.editor.interaction.active &&
        frame.editor.input_state != frame.editor.interaction.input_state) {
        violations.emplace_back("interaction input state does not match the active view");
    }
    if (frame.editor.interaction.active && frame.editor.interaction.buffer_name.empty()) {
        violations.emplace_back("interaction buffer name is empty");
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
    const ui::Region* active_text_area = nullptr;
    for (const ui::Region& region : frame.scene.regions) {
        if (region.active && region.role == ui::RegionRole::TextArea) {
            active_text_area = &region;
            break;
        }
    }
    const float active_grid_offset =
        active_text_area != nullptr &&
                active_text_area->vertical_anchor == ui::VerticalAnchor::PaneGrid
            ? active_text_area->content_offset_rows
            : frame.scene.grid_offset_rows;
    if (std::abs(active_grid_offset + frame.editor.viewport.top_line_offset) > 0.0001F) {
        violations.emplace_back("active scene grid offset does not match editor viewport");
    }
    if (!frame.scene.panes.empty()) {
        const std::size_t active_panes = static_cast<std::size_t>(std::ranges::count_if(
            frame.scene.panes, [](const ui::ScenePane& pane) { return pane.active; }));
        if (active_panes != 1) {
            violations.emplace_back("workspace must have exactly one active pane");
        }
    }
    if (frame.scene.cursor_visible &&
        (frame.scene.cursor_row < 1 || frame.scene.cursor_row > frame.scene.rows ||
         frame.scene.cursor_col < 1 || frame.scene.cursor_col > frame.scene.cols)) {
        violations.emplace_back("scene cursor is outside the scene");
    }
    std::vector<std::string> view_ids;
    view_ids.reserve(frame.scene.regions.size());
    for (const ui::Region& region : frame.scene.regions) {
        const std::string view_id = ui::region_view_id(region);
        if (std::ranges::find(view_ids, view_id) != view_ids.end()) {
            violations.push_back(std::format("duplicate scene view id '{}'", view_id));
        } else {
            view_ids.push_back(view_id);
        }
        if (region.rect.row < 0 || region.rect.col < 0 || region.rect.rows < 0 ||
            region.rect.cols < 0 || region.rect.row + region.rect.rows > frame.scene.rows ||
            region.rect.col + region.rect.cols > frame.scene.cols) {
            violations.push_back(
                std::format("region:{} is outside the scene", region_role_name(region.role)));
        }
        if (!std::isfinite(region.content_offset_rows) || region.content_offset_rows > 0.0F ||
            region.content_offset_rows <= -1.0F) {
            violations.push_back(std::format("region:{} has an invalid content offset",
                                             region_role_name(region.role)));
        }
        if (!region.pane_id.empty()) {
            const auto pane =
                std::ranges::find_if(frame.scene.panes, [&](const ui::ScenePane& candidate) {
                    return candidate.id == region.pane_id;
                });
            if (pane == frame.scene.panes.end()) {
                violations.push_back(
                    std::format("region:{} names an unknown pane", region_role_name(region.role)));
            } else if (pane->active != region.active) {
                violations.push_back(std::format("region:{} active state differs from its pane",
                                                 region_role_name(region.role)));
            }
        }
        for (std::size_t index = 0; index < region.primitives().size(); ++index) {
            const ui::Prim& prim = region.primitives()[index];
            if (prim.row < 0 || prim.row >= region.rect.rows || prim.col < 0 ||
                prim.col >= region.rect.cols) {
                violations.push_back(std::format("region:{}/prim:{} starts outside its region",
                                                 region_role_name(region.role), index));
            }
        }
        if (const ui::Region::DocumentMapping* mapping = region.document_mapping()) {
            if (region.role != ui::RegionRole::TextArea &&
                region.role != ui::RegionRole::LineNumbers &&
                region.role != ui::RegionRole::ChangeSigns) {
                violations.emplace_back("document mapping is outside a document region");
            }
            if (mapping->first_display_column && *mapping->first_display_column < 0) {
                violations.emplace_back("document mapping starts before display column zero");
            }
        }
        if (region.role == ui::RegionRole::TextArea && region.active) {
            const ui::Region::DocumentMapping* mapping = region.document_mapping();
            if (mapping == nullptr || !mapping->first_display_column) {
                violations.emplace_back("scene text area has no document coordinate mapping");
            } else if (mapping->first_line != frame.editor.viewport.top_line ||
                       *mapping->first_display_column != frame.editor.viewport.left_column) {
                violations.emplace_back(
                    "scene document mapping does not match the editor viewport");
            }
        }
        if (const ui::Region::PopupContent* popup = region.popup()) {
            if (region.role != ui::RegionRole::Popup) {
                violations.emplace_back("structured popup content is outside the popup region");
            }
            if (popup->input_cursor &&
                (!popup->input || *popup->input_cursor > popup->input->size())) {
                violations.emplace_back("popup input cursor is past the input end");
            }
            if (popup->presentation == ui::Region::PopupPresentation::Completion && popup->input) {
                violations.emplace_back("completion popup owns a duplicate input surface");
            }
            if (popup->first_item > popup->total_items ||
                popup->items.size() > popup->total_items - popup->first_item) {
                violations.emplace_back("popup viewport is outside the item range");
            }
            if (popup->selected_item && *popup->selected_item >= popup->total_items) {
                violations.emplace_back("popup selection is outside the item range");
            } else if (popup->selected_item &&
                       (*popup->selected_item < popup->first_item ||
                        *popup->selected_item >= popup->first_item + popup->items.size())) {
                violations.emplace_back("popup selection is outside the visible viewport");
            }
        }
        if (const ModelineContent* status = region.status()) {
            (void)status;
            if (region.role != ui::RegionRole::StatusBar) {
                violations.emplace_back("structured status content is outside the status region");
            }
            if (std::ranges::any_of(status->segments, [](const ModelineSegment& segment) {
                    return segment.text.empty();
                })) {
                violations.emplace_back("modeline segment text is empty");
            }
        }
        if (const ui::Region::EchoContent* echo = region.echo()) {
            if (region.role != ui::RegionRole::EchoArea) {
                violations.emplace_back("structured echo content is outside the echo region");
            }
            if (echo->cursor_byte && *echo->cursor_byte > echo->text.size()) {
                violations.emplace_back("echo cursor byte is past the text end");
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
    const ui::Region* text_area = frame.scene.find(ui::RegionRole::TextArea);
    if (text_area != nullptr && !frame.render.document_layout) {
        violations.emplace_back("scene text area has no render document layout");
    }
    if (frame.render.document_layout) {
        const DocumentLayoutSnapshot& layout = *frame.render.document_layout;
        if (text_area == nullptr) {
            violations.emplace_back("render document layout has no scene text area");
        } else if (layout.lines.size() != static_cast<std::size_t>(text_area->rect.rows)) {
            violations.emplace_back("render document lines do not match the text area");
        }
        const bool cursor_in_document =
            text_area != nullptr && frame.scene.cursor_visible &&
            frame.scene.cursor_row - 1 >= text_area->rect.row &&
            frame.scene.cursor_row - 1 < text_area->rect.row + text_area->rect.rows &&
            frame.scene.cursor_col - 1 >= text_area->rect.col &&
            frame.scene.cursor_col - 1 < text_area->rect.col + text_area->rect.cols;
        if (cursor_in_document) {
            const int expected_row = frame.scene.cursor_row - 1 - text_area->rect.row;
            const int expected_column = frame.scene.cursor_col - 1 - text_area->rect.col;
            if (layout.cursor_row != expected_row || layout.cursor_column != expected_column ||
                !layout.cursor_rect) {
                violations.emplace_back("render document cursor does not match the scene cursor");
            }
            const float expected_grid_x =
                static_cast<float>((frame.scene.cursor_col - 1) * frame.render.cell_width);
            if (std::abs(layout.grid_cursor_x - expected_grid_x) > 0.01F) {
                violations.emplace_back("render document grid cursor coordinate is inconsistent");
            }
        } else if (layout.cursor_row || layout.cursor_column || layout.cursor_rect) {
            violations.emplace_back("render document exposes a cursor outside the text area");
        }
        if (!std::isfinite(layout.bounds.x) || !std::isfinite(layout.bounds.y) ||
            !std::isfinite(layout.bounds.width) || !std::isfinite(layout.bounds.height) ||
            !std::isfinite(layout.cursor_advance) || !std::isfinite(layout.grid_cursor_x)) {
            violations.emplace_back("render document layout is not finite");
        }
        if (layout.cursor_rect &&
            std::abs(layout.cursor_rect->x - (layout.bounds.x + layout.cursor_advance)) > 0.01F) {
            violations.emplace_back("render document cursor does not match shaped advance");
        }
        for (std::size_t index = 0; index < layout.lines.size(); ++index) {
            const DocumentLineLayoutSnapshot& line = layout.lines[index];
            if (line.row != static_cast<int>(index) || line.end_column < 0 ||
                !std::isfinite(line.origin_x) || !std::isfinite(line.advance) ||
                line.advance < 0.0F) {
                violations.emplace_back("render document line layout is invalid");
                break;
            }
        }
    }
    const ui::Region* popup_region = frame.scene.find(ui::RegionRole::Popup);
    const ui::Region::PopupContent* popup =
        popup_region != nullptr ? popup_region->popup() : nullptr;
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
    const ui::Region::EchoContent* echo = echo_region != nullptr ? echo_region->echo() : nullptr;
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
    if (frame.scene.cursor_visible && text_area) {
        const int cursor_row = frame.scene.cursor_row - 1;
        if (cursor_row >= text_area->rect.row &&
            cursor_row < text_area->rect.row + text_area->rect.rows &&
            frame.render.display_scale > 0.0F && frame.render.cell_height > 0) {
            const float logical_height =
                static_cast<float>(frame.render.output_height) / frame.render.display_scale;
            const ui::ScenePixelLayout layout(
                frame.scene,
                {.cell_height = static_cast<float>(frame.render.cell_height),
                 .viewport_height = logical_height,
                 .footer_heights = ui::editor_footer_heights(
                     static_cast<float>(frame.render.cell_height), frame.render.metrics)},
                static_cast<float>(frame.render.cell_width));
            const ui::ScenePixelRect text_bounds = layout.region_rect(*text_area);
            const float top =
                text_bounds.y +
                static_cast<float>(cursor_row - text_area->rect.row) *
                    static_cast<float>(frame.render.cell_height) +
                text_area->content_offset_rows * static_cast<float>(frame.render.cell_height);
            const float bottom = top + static_cast<float>(frame.render.cell_height);
            if (top < text_bounds.y - 0.01F || bottom > text_bounds.bottom() + 0.01F) {
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
    if (animation.active) {
        std::string_view expected_cursor_owner = "other";
        if (!frame.scene.cursor_visible) {
            expected_cursor_owner = "none";
        } else if (frame.render.popup_layout && frame.render.popup_layout->cursor_rect) {
            expected_cursor_owner = "popup";
        } else if (frame.render.echo_layout && frame.render.echo_layout->cursor_rect) {
            expected_cursor_owner = "echo";
        } else if (frame.render.document_layout && frame.render.document_layout->cursor_rect) {
            expected_cursor_owner = "document";
        }
        if (animation.cursor_owner != expected_cursor_owner) {
            violations.emplace_back("render animation cursor owner does not match shaped layout");
        }
        if (frame.scene.cursor_visible && !animation.cursor_rect) {
            violations.emplace_back("render animation has no presented cursor bounds");
        } else if (!frame.scene.cursor_visible && animation.cursor_rect) {
            violations.emplace_back("render animation has cursor bounds for a hidden cursor");
        }
        if (animation.cursor_rect) {
            const LogicalPixelRectSnapshot& cursor = *animation.cursor_rect;
            const float logical_width =
                frame.render.display_scale > 0.0F
                    ? static_cast<float>(frame.render.output_width) / frame.render.display_scale
                    : 0.0F;
            const float logical_height =
                frame.render.display_scale > 0.0F
                    ? static_cast<float>(frame.render.output_height) / frame.render.display_scale
                    : 0.0F;
            if (!std::isfinite(cursor.x) || !std::isfinite(cursor.y) ||
                !std::isfinite(cursor.width) || !std::isfinite(cursor.height) ||
                cursor.width <= 0.0F || cursor.height <= 0.0F) {
                violations.emplace_back("render animation cursor bounds are invalid");
            } else if (cursor.x < -0.01F || cursor.y < -0.01F ||
                       cursor.x + cursor.width > logical_width + 0.01F ||
                       cursor.y + cursor.height > logical_height + 0.01F) {
                violations.emplace_back("render animation cursor is outside the output");
            }
        }
        if (animation.cursor && animation.cursor_owner == "document") {
            if (!animation.cursor_rect || !animation.active_line_y) {
                violations.emplace_back(
                    "render document cursor animation has no synchronized active line");
            } else if (std::abs(animation.cursor_rect->y - *animation.active_line_y) > 0.01F) {
                violations.emplace_back("render document cursor and active line are out of phase");
            }
        }
    }
    if (animation.scroll) {
        if (!std::isfinite(animation.visual_scroll_top) ||
            !std::isfinite(animation.target_scroll_top)) {
            violations.emplace_back("render scroll positions are not finite");
        }
        const LogicalPixelRectSnapshot* view_cursor = nullptr;
        if (frame.render.popup_layout && frame.render.popup_layout->cursor_rect) {
            view_cursor = &*frame.render.popup_layout->cursor_rect;
        } else if (frame.render.echo_layout && frame.render.echo_layout->cursor_rect) {
            view_cursor = &*frame.render.echo_layout->cursor_rect;
        } else if (frame.render.document_layout && frame.render.document_layout->cursor_rect) {
            view_cursor = &*frame.render.document_layout->cursor_rect;
        }
        const float document_offset_y =
            (animation.target_scroll_top - animation.visual_scroll_top) *
            static_cast<float>(frame.render.cell_height);
        if (view_cursor && animation.cursor_rect) {
            const float expected_cursor_y =
                view_cursor->y + (animation.cursor_owner == "document" ? document_offset_y : 0.0F);
            if (std::abs(animation.cursor_rect->x - view_cursor->x) > 0.01F ||
                std::abs(animation.cursor_rect->y - expected_cursor_y) > 0.01F ||
                std::abs(animation.cursor_rect->width - view_cursor->width) > 0.01F ||
                std::abs(animation.cursor_rect->height - view_cursor->height) > 0.01F) {
                violations.emplace_back("render scroll cursor does not match the visual viewport");
            }
        }
        if (animation.active_line_y && !std::isfinite(*animation.active_line_y)) {
            violations.emplace_back("render animated active line position is not finite");
        }
        if (frame.scene.active_text_row && frame.render.display_scale > 0.0F &&
            frame.render.cell_height > 0) {
            const float logical_height =
                static_cast<float>(frame.render.output_height) / frame.render.display_scale;
            const ui::SceneVerticalLayout layout(
                frame.scene,
                {.cell_height = static_cast<float>(frame.render.cell_height),
                 .viewport_height = logical_height,
                 .footer_heights = ui::editor_footer_heights(
                     static_cast<float>(frame.render.cell_height), frame.render.metrics)});
            const float expected_active_line_y =
                layout.row_top(*frame.scene.active_text_row) + document_offset_y;
            if (!animation.active_line_y ||
                std::abs(*animation.active_line_y - expected_active_line_y) > 0.01F) {
                violations.emplace_back(
                    "render animated active line does not match the visual viewport");
            }
            if (animation.cursor_constrained && animation.cursor_owner == "document" &&
                animation.cursor_rect &&
                (animation.cursor_rect->y < -0.01F ||
                 animation.cursor_rect->y + animation.cursor_rect->height >
                     layout.grid_clip_bottom() + 0.01F)) {
                violations.emplace_back("render scroll cursor is outside the document grid");
            }
        } else if (animation.active_line_y) {
            violations.emplace_back("render scroll has an active line without current view state");
        }
        if (animation.layers.empty() || animation.layers.size() > 2) {
            violations.emplace_back("render scroll does not have adjacent viewport layers");
        }
        float previous_scroll_top = -std::numeric_limits<float>::infinity();
        float previous_clip_bottom = 0.0F;
        std::optional<float> grid_bottom;
        if (frame.render.display_scale > 0.0F && frame.render.cell_height > 0) {
            const float logical_height =
                static_cast<float>(frame.render.output_height) / frame.render.display_scale;
            const ui::SceneVerticalLayout layout(
                frame.scene,
                {.cell_height = static_cast<float>(frame.render.cell_height),
                 .viewport_height = logical_height,
                 .footer_heights = ui::editor_footer_heights(
                     static_cast<float>(frame.render.cell_height), frame.render.metrics)});
            grid_bottom = layout.grid_clip_bottom();
        }
        for (const RenderScrollLayerSnapshot& layer : animation.layers) {
            if (!std::isfinite(layer.scroll_top) || !std::isfinite(layer.grid_offset_y) ||
                !std::isfinite(layer.clip_top) || !std::isfinite(layer.clip_bottom)) {
                violations.emplace_back("render scroll layer is not finite");
                continue;
            }
            if (layer.scroll_top < previous_scroll_top) {
                violations.emplace_back("render scroll layers are not document-ordered");
            }
            const float expected_offset = (layer.scroll_top - animation.visual_scroll_top) *
                                          static_cast<float>(frame.render.cell_height);
            if (std::abs(layer.grid_offset_y - expected_offset) > 0.01F) {
                violations.emplace_back(
                    "render scroll layer offset does not match document position");
            }
            if (layer.clip_bottom <= layer.clip_top || layer.clip_top < -0.01F ||
                (grid_bottom && layer.clip_bottom > *grid_bottom + 0.01F)) {
                violations.emplace_back("render scroll layer clip is outside the grid");
            }
            if (std::abs(layer.clip_top - previous_clip_bottom) > 0.01F) {
                violations.emplace_back("render scroll layer clips leave a gap or overlap");
            }
            previous_scroll_top = layer.scroll_top;
            previous_clip_bottom = layer.clip_bottom;
        }
        if (grid_bottom && !animation.layers.empty() &&
            std::abs(previous_clip_bottom - *grid_bottom) > 0.01F) {
            violations.emplace_back("render scroll layer clips do not cover the grid");
        }
        if (!animation.layers.empty() &&
            (animation.layers.front().scroll_top > animation.visual_scroll_top + 0.0001F ||
             animation.layers.back().scroll_top < animation.visual_scroll_top - 0.0001F)) {
            violations.emplace_back("render scroll layers do not bracket the visual position");
        }
    } else if (!animation.layers.empty()) {
        violations.emplace_back("idle scroll animation retains viewport layers");
    }
    if (animation.active != (animation.scroll || animation.cursor)) {
        violations.emplace_back("render animation activity flags are inconsistent");
    }
    if (animation.cursor_constrained &&
        (!animation.scroll || animation.cursor_owner != "document" || !animation.cursor_rect)) {
        violations.emplace_back("render cursor constraint has no document scroll cursor");
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
            primitive.primitive_index >= frame.scene.regions[primitive.region_index].item_count()) {
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
    } else if (path == "editor.selection") {
        append_selection(output, frame.editor.selection);
    } else if (path == "editor.position_hints") {
        append_position_hints(output, frame.editor.position_hints);
    } else if (path == "editor.viewport") {
        output = std::format("{{\"top_line\":{},\"top_line_offset\":{},\"left_column\":{}}}",
                             frame.editor.viewport.top_line, frame.editor.viewport.top_line_offset,
                             frame.editor.viewport.left_column);
    } else if (path == "editor.line_signs") {
        append_line_signs(output, frame.editor.line_signs);
    } else if (path == "editor.command_loop") {
        append_command_loop(output, frame.editor.command_loop);
    } else if (path == "editor.scripting") {
        append_scripting(output, frame.editor.scripting);
    } else if (path == "editor.interaction") {
        append_interaction(output, frame.editor.interaction);
    } else if (path == "editor.completion") {
        append_completion(output, frame.editor.completion);
    } else if (path == "editor.lsp") {
        append_lsp(output, frame.editor.lsp);
    } else if (path == "editor.buffers") {
        append_buffers(output, frame.editor.buffers);
    } else if (path == "editor.windows") {
        append_windows(output, frame.editor.windows);
    } else if (path == "editor.workbenches") {
        append_workbenches(output, frame.editor.workbenches);
    } else if (path == "editor.projects") {
        append_projects(output, frame.editor.projects);
    } else if (path == "editor.location") {
        append_location(output, frame.editor.location_at_caret);
    } else if (path == "editor.location_navigation") {
        append_location_navigation(output, frame.editor.location_navigation);
    } else if (path == "editor.jumps") {
        append_jumps(output, frame.editor.jumps);
    } else if (path == "editor.focus") {
        output =
            std::format("{{\"window\":{{\"slot\":{},\"generation\":{}}},\"target\":",
                        frame.editor.active_window_slot, frame.editor.active_window_generation);
        append_json_string(output, frame.editor.input_focus);
        output += ",\"stack\":[{\"target\":\"window\",\"window\":{";
        output += std::format("\"slot\":{},\"generation\":{}}}", frame.editor.active_window_slot,
                              frame.editor.active_window_generation);
        if (frame.editor.interaction.active) {
            output += ",{\"target\":\"minibuffer\",\"window\":{";
            output +=
                std::format("\"slot\":{},\"generation\":{}}}", frame.editor.interaction.window_slot,
                            frame.editor.interaction.window_generation);
        }
        output.push_back(']');
        output.push_back('}');
    } else if (path == "editor.input_state") {
        output = "{\"strategy\":";
        append_json_string(output, frame.editor.input_strategy);
        output += ",\"name\":";
        append_json_string(output, frame.editor.input_state);
        output += ",\"cursor_shape\":";
        append_json_string(output, frame.editor.input_cursor_shape);
        output += ",\"indicator\":";
        append_json_string(output, frame.editor.input_state_indicator);
        output += ",\"text_input\":";
        append_json_string(output, frame.editor.text_input_policy);
        output += ",\"text_command\":";
        append_json_string(output, frame.editor.text_input_command);
        output += ",\"text_command_available\":";
        append_bool(output, frame.editor.text_input_command_available);
        output += ",\"selection_after_edit\":";
        append_json_string(output, frame.editor.selection_after_edit);
        output += ",\"handler\":";
        append_bool(output, frame.editor.input_state_handler);
        output += ",\"on_enter\":";
        append_bool(output, frame.editor.input_state_on_enter);
        output += ",\"on_exit\":";
        append_bool(output, frame.editor.input_state_on_exit);
        output += ",\"position_hints_provider\":";
        append_bool(output, frame.editor.position_hints.provider);
        output.push_back('}');
    } else if (path == "scene") {
        append_scene(output, frame.scene);
    } else if (path == "scene.cursor") {
        output =
            std::format("{{\"row\":{},\"col\":{},\"visible\":{},\"shape\":", frame.scene.cursor_row,
                        frame.scene.cursor_col, frame.scene.cursor_visible);
        append_json_string(output, cursor_shape_name(frame.scene.cursor_shape));
        output.push_back('}');
    } else if (path == "scene.panes") {
        output.push_back('[');
        for (std::size_t index = 0; index < frame.scene.panes.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const ui::ScenePane& pane = frame.scene.panes[index];
            output += "{\"id\":";
            append_json_string(output, pane.id);
            output += ",\"rect\":";
            append_rect(output, pane.rect);
            output += ",\"active\":";
            append_bool(output, pane.active);
            output.push_back('}');
        }
        output.push_back(']');
    } else if (path == "scene.dividers") {
        output.push_back('[');
        for (std::size_t index = 0; index < frame.scene.dividers.size(); ++index) {
            if (index != 0) {
                output.push_back(',');
            }
            const ui::SceneDivider& divider = frame.scene.dividers[index];
            output += "{\"id\":";
            append_json_string(output, divider.id);
            output += ",\"axis\":";
            append_json_string(output, divider.axis == ui::DividerAxis::Horizontal ? "horizontal"
                                                                                   : "vertical");
            output += std::format(",\"position\":{},\"start\":{},\"length\":{}}}", divider.position,
                                  divider.start, divider.length);
        }
        output.push_back(']');
    } else if (path == "scene.view_tree") {
        append_view_tree(output, frame.scene);
    } else if (path == "render") {
        append_render(output, frame.render);
    } else if (path == "render.font_metrics") {
        append_font_metrics(output, frame.render.font_metrics);
    } else if (path == "render.animation") {
        append_render_animation(output, frame.render.animation);
    } else if (path == "render.damage") {
        append_render_damage(output, frame.render.damage);
    } else if (path == "render.timings") {
        append_render_timings(output, frame.render.timings);
    } else if (path == "render.document_layout") {
        append_document_layout(output, frame.render.document_layout);
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
    const ui::ScenePixelLayout pixel_layout(
        frame.scene,
        {.cell_height = static_cast<float>(frame.render.cell_height),
         .viewport_height = logical_height,
         .footer_heights = ui::editor_footer_heights(static_cast<float>(frame.render.cell_height),
                                                     frame.render.metrics)},
        static_cast<float>(frame.render.cell_width));
    const ui::SceneVerticalLayout& vertical_layout = pixel_layout.vertical();
    int cell_row = vertical_layout.row_at(logical_y);
    const ui::ViewTree view_tree(frame.scene);
    const auto map_layer_row = [&](ui::ViewLayer layer) {
        const ui::ViewLayerNode& nodes = view_tree.layer(layer);
        for (auto iterator = nodes.children.rbegin(); iterator != nodes.children.rend();
             ++iterator) {
            const ui::Region& region = frame.scene.regions[iterator->region_index];
            const ui::ScenePixelRect bounds = pixel_layout.region_rect(region);
            if (!bounds.contains(logical_x, logical_y)) {
                continue;
            }
            if (region.vertical_anchor == ui::VerticalAnchor::PaneGrid) {
                const float local_y =
                    (logical_y - bounds.y) / static_cast<float>(frame.render.cell_height) -
                    region.content_offset_rows;
                const int local_row = std::clamp(static_cast<int>(std::floor(local_y)), 0,
                                                 std::max(0, region.rect.rows - 1));
                cell_row = region.rect.row + local_row;
            } else if (region.vertical_anchor == ui::VerticalAnchor::Cell) {
                cell_row = region.rect.row;
            } else if (region.vertical_anchor == ui::VerticalAnchor::Overlay) {
                cell_row = static_cast<int>(
                    std::floor(logical_y / static_cast<float>(frame.render.cell_height)));
            }
            return true;
        }
        return false;
    };
    (void)(map_layer_row(ui::ViewLayer::Overlay) || map_layer_row(ui::ViewLayer::Chrome) ||
           map_layer_row(ui::ViewLayer::Grid));

    std::string output =
        std::format("{{\"window\":{{\"x\":{},\"y\":{}}},\"cell\":{{\"row\":{},\"col\":{}}}",
                    window_x, window_y, cell_row, cell_col);
    if (cell_row >= frame.scene.rows || cell_col >= frame.scene.cols) {
        output += ",\"region\":null,\"target\":null,\"prim\":null,\"render\":null}";
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
            iterator->primitive_index < frame.scene.regions[iterator->region_index].item_count()) {
            hit_render = &*iterator;
            hit_region_index = iterator->region_index;
            hit_region = &frame.scene.regions[hit_region_index];
            break;
        }
    }
    if (!hit_region) {
        const auto find_in_layer = [&](ui::ViewLayer layer) {
            const ui::ViewLayerNode& layer_node = view_tree.layer(layer);
            for (auto iterator = layer_node.children.rbegin();
                 iterator != layer_node.children.rend(); ++iterator) {
                const ui::Region& region = frame.scene.regions[iterator->region_index];
                if (cell_row >= region.rect.row && cell_row < region.rect.row + region.rect.rows &&
                    cell_col >= region.rect.col && cell_col < region.rect.col + region.rect.cols) {
                    hit_region = &region;
                    hit_region_index = iterator->region_index;
                    return true;
                }
            }
            return false;
        };
        (void)(find_in_layer(ui::ViewLayer::Overlay) || find_in_layer(ui::ViewLayer::Chrome) ||
               find_in_layer(ui::ViewLayer::Grid));
    }

    if (!hit_region) {
        output += ",\"region\":null,\"target\":null,\"prim\":null,\"render\":null}";
        return {true, std::move(output)};
    }

    output += ",\"region\":";
    append_json_string(output, region_id(*hit_region));
    const int local_row = cell_row - hit_region->rect.row;
    const int local_col = cell_col - hit_region->rect.col;
    const bool pixel_overlay_hit =
        hit_render && hit_region->vertical_anchor == ui::VerticalAnchor::Overlay;
    if (pixel_overlay_hit) {
        output += ",\"local_cell\":null";
    } else {
        output += std::format(",\"local_cell\":{{\"row\":{},\"col\":{}}}", local_row, local_col);
    }

    std::optional<ui::Prim> hit_primitive;
    std::size_t hit_primitive_index = hit_region->item_count();
    if (hit_render) {
        hit_primitive_index = hit_render->primitive_index;
        hit_primitive = inspection_primitive(*hit_region, hit_primitive_index);
    } else {
        for (std::size_t index = 0; index < hit_region->primitives().size(); ++index) {
            const ui::Prim& prim = hit_region->primitives()[index];
            const int width = std::max(1, ui::display_width(prim.text));
            if (prim.row == local_row && local_col >= prim.col && local_col < prim.col + width) {
                hit_primitive = prim;
                hit_primitive_index = index;
            }
        }
        if (!hit_primitive && hit_region->primitives().empty()) {
            const std::size_t semantic_index =
                hit_region->popup() != nullptr ? static_cast<std::size_t>(std::max(0, local_row))
                                               : 0;
            hit_primitive = inspection_primitive(*hit_region, semantic_index);
            if (hit_primitive) {
                hit_primitive_index = semantic_index;
            }
        }
    }
    const std::optional<ui::HitTarget> target = ui::resolve_hit_target(
        frame.scene,
        {.region_index = hit_region_index,
         .scene_cell = pixel_overlay_hit
                           ? std::nullopt
                           : std::optional(ui::CellPoint{.row = cell_row, .column = cell_col}),
         .local_cell = pixel_overlay_hit
                           ? std::nullopt
                           : std::optional(ui::CellPoint{.row = local_row, .column = local_col}),
         .content_index = hit_primitive_index < hit_region->item_count()
                              ? std::optional(hit_primitive_index)
                              : std::nullopt});
    output += ",\"target\":";
    if (target) {
        append_hit_target(output, *target);
    } else {
        output += "null";
    }
    output += ",\"prim\":";
    if (hit_primitive) {
        append_prim(output, *hit_region, *hit_primitive);
    } else {
        output += "null";
    }
    output += ",\"render\":";
    if (hit_primitive) {
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
    case ui::RegionRole::Documentation:
        return "documentation";
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
    case ui::StyleClass::DiagnosticError:
        return "diagnostic-error";
    case ui::StyleClass::DiagnosticWarning:
        return "diagnostic-warning";
    case ui::StyleClass::DiagnosticInformation:
        return "diagnostic-information";
    case ui::StyleClass::DiagnosticHint:
        return "diagnostic-hint";
    case ui::StyleClass::StatusBar:
        return "status-bar";
    case ui::StyleClass::StatusKey:
        return "status-key";
    case ui::StyleClass::Message:
        return "message";
    case ui::StyleClass::Popup:
        return "popup";
    case ui::StyleClass::PositionHint:
        return "position-hint";
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
    output << "    selection active=" << (frame.editor.selection.active ? "true" : "false")
           << " ranges=" << frame.editor.selection.ranges.size()
           << " primary=" << frame.editor.selection.primary
           << " history=" << frame.editor.selection.history_depth << " meta=\""
           << printable(frame.editor.selection.metadata) << "\"\n";
    output << "    position-hints provider="
           << (frame.editor.position_hints.provider ? "true" : "false")
           << " items=" << frame.editor.position_hints.items.size();
    if (frame.editor.position_hints.error) {
        output << " error=\"" << printable(*frame.editor.position_hints.error) << '"';
    }
    output << '\n';
    output << "    focus=" << printable(frame.editor.input_focus)
           << " strategy=" << printable(frame.editor.input_strategy)
           << " state=" << printable(frame.editor.input_state)
           << " cursor=" << printable(frame.editor.input_cursor_shape) << " indicator=\""
           << printable(frame.editor.input_state_indicator) << '"'
           << " text-input=" << printable(frame.editor.text_input_policy)
           << " text-command=" << printable(frame.editor.text_input_command)
           << " text-command-available="
           << (frame.editor.text_input_command_available ? "true" : "false")
           << " selection-after-edit=" << printable(frame.editor.selection_after_edit)
           << " handler=" << (frame.editor.input_state_handler ? "true" : "false")
           << " on-enter=" << (frame.editor.input_state_on_enter ? "true" : "false")
           << " on-exit=" << (frame.editor.input_state_on_exit ? "true" : "false")
           << " window:" << frame.editor.active_window_slot << ':'
           << frame.editor.active_window_generation << '\n';
    output << "    command keymaps=" << frame.editor.command_loop.keymaps.size() << " pending=\""
           << printable(frame.editor.command_loop.pending_keys) << "\" owner=\""
           << printable(frame.editor.command_loop.pending_keymap) << "\" state=\""
           << printable(frame.editor.command_loop.pending_input_state) << "\" prefix=\""
           << printable(frame.editor.command_loop.prefix_text) << "\" last=\""
           << printable(frame.editor.command_loop.last_command) << "\"\n";
    output << "    scripting=" << printable(frame.editor.scripting.engine) << ' '
           << printable(frame.editor.scripting.version)
           << " commands=" << frame.editor.scripting.scripted_commands
           << " command-revision=" << frame.editor.scripting.command_revision
           << " providers=" << frame.editor.scripting.scripted_providers
           << " provider-revision=" << frame.editor.scripting.provider_revision
           << " binding-revision=" << frame.editor.scripting.binding_revision
           << " input-states=" << frame.editor.scripting.scripted_input_states
           << " input-strategies=" << frame.editor.scripting.scripted_input_strategies
           << " input-state-revision=" << frame.editor.scripting.input_state_revision
           << " modes=" << frame.editor.scripting.scripted_modes
           << " mode-revision=" << frame.editor.scripting.mode_revision
           << " file-mode-rules=" << frame.editor.scripting.scripted_file_mode_rules
           << " project-providers=" << frame.editor.scripting.scripted_project_providers
           << " async-tasks=" << frame.editor.scripting.outstanding_async_tasks
           << " resource-policy-revision=" << frame.editor.scripting.resource_policy_revision
           << " extensions=" << frame.editor.scripting.extensions.size();
    if (frame.editor.scripting.last_error) {
        output << " error=\"" << printable(*frame.editor.scripting.last_error) << '"';
    }
    output << '\n';
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
           << " keymap=" << printable(frame.editor.interaction.keymap)
           << " input-state=" << printable(frame.editor.interaction.input_state)
           << " buffer=" << printable(frame.editor.interaction.buffer_name)
           << " provider=" << printable(frame.editor.interaction.provider)
           << " input-cursor=" << frame.editor.interaction.input_cursor
           << " history=" << frame.editor.interaction.history_entries;
    if (frame.editor.interaction.history_index) {
        output << '@' << *frame.editor.interaction.history_index;
    }
    output << " candidates=" << frame.editor.interaction.candidates.size() << '\n';
    output << "    buffers=" << frame.editor.buffers.size() << '\n';
    for (const OpenBufferStateSnapshot& buffer : frame.editor.buffers) {
        output << "      buffer:" << buffer.buffer_slot << ':' << buffer.buffer_generation
               << (buffer.active ? " active" : "") << (buffer.modified ? " modified" : "")
               << " name=\"" << printable(buffer.name) << "\" resource=\""
               << printable(buffer.resource) << "\" class=" << printable(buffer.interaction_class)
               << " initial-state=" << printable(buffer.initial_input_state)
               << " completion-auto=" << (buffer.completion_auto ? "true" : "false")
               << " completion-providers=";
        for (std::size_t index = 0; index < buffer.completion_providers.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << printable(buffer.completion_providers[index]);
        }
        output << '\n';
    }
    output << "    windows=" << frame.editor.windows.size() << '\n';
    for (const OpenWindowStateSnapshot& window : frame.editor.windows) {
        output << "      window:" << window.window_slot << ':' << window.window_generation
               << (window.active ? " active" : "") << " view:" << window.view_slot << ':'
               << window.view_generation << " buffer:" << window.buffer_slot << ':'
               << window.buffer_generation << " role=" << printable(window.role)
               << (window.pinned ? " pinned" : "")
               << (window.created_by_policy ? " policy-created" : "") << " input-states=";
        for (std::size_t index = 0; index < window.input_states.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << printable(window.input_states[index]);
        }
        output << '\n';
    }
    output << "    workbenches=" << frame.editor.workbenches.size() << '\n';
    for (const WorkbenchStateSnapshot& workbench : frame.editor.workbenches) {
        output << "      workbench:" << workbench.workbench.slot << ':'
               << workbench.workbench.generation << (workbench.active ? " active" : "")
               << " name=\"" << printable(workbench.name) << "\" scope=" << workbench.scope.size()
               << " mru=" << workbench.mru.size() << " windows=" << workbench.windows.size()
               << " active-window:" << workbench.active_window.slot << ':'
               << workbench.active_window.generation << " slots=";
        for (std::size_t index = 0; index < workbench.slots.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << printable(workbench.slots[index].role) << "->"
                   << workbench.slots[index].window.slot << ':'
                   << workbench.slots[index].window.generation;
        }
        output << '\n';
    }
    output << "  scene " << frame.scene.cols << 'x' << frame.scene.rows << " cursor=";
    if (frame.scene.cursor_visible) {
        output << frame.scene.cursor_row << ':' << frame.scene.cursor_col << ' '
               << cursor_shape_name(frame.scene.cursor_shape);
    } else {
        output << "hidden";
    }
    output << '\n';
    for (const ui::ScenePane& pane : frame.scene.panes) {
        output << "    pane " << printable(pane.id) << (pane.active ? " active" : " inactive")
               << " rect=(" << pane.rect.row << ',' << pane.rect.col << ' ' << pane.rect.rows << 'x'
               << pane.rect.cols << ")\n";
    }
    const ui::ViewTree view_tree(frame.scene);
    for (const ui::ViewLayerNode& layer : view_tree.layers()) {
        output << "    " << layer.id << " layer=" << view_layer_name(layer.layer) << '\n';
        for (const ui::ViewNode& node : layer.children) {
            const ui::Region& region = frame.scene.regions[node.region_index];
            output << "    " << region_id(region) << " role=" << region_role_name(region.role)
                   << " rev=" << region.revision << " rect=(" << region.rect.row << ','
                   << region.rect.col << ' ' << region.rect.rows << 'x' << region.rect.cols
                   << ") anchor=" << vertical_anchor_name(region.vertical_anchor)
                   << " pane=" << printable(region.pane_id)
                   << (region.active ? " active" : " inactive")
                   << " content-offset=" << region.content_offset_rows
                   << " items=" << region.item_count() << '\n';
            if (const ui::Region::PopupContent* popup = region.popup()) {
                output << "      list presentation="
                       << (popup->presentation == ui::Region::PopupPresentation::Completion
                               ? "completion"
                               : "band")
                       << " first=" << popup->first_item << " visible=" << popup->items.size()
                       << " total=" << popup->total_items << " selected=";
                if (popup->selected_item) {
                    output << *popup->selected_item;
                } else {
                    output << "none";
                }
                output << '\n';
            }
            if (const ui::Region::EchoContent* echo = region.echo()) {
                output << "      echo bytes=" << echo->text.size() << " cursor=";
                if (echo->cursor_byte) {
                    output << *echo->cursor_byte;
                } else {
                    output << "none";
                }
                output << '\n';
            }
            if (const ModelineContent* status = region.status()) {
                output << "      modeline segments=" << status->segments.size() << '\n';
                for (const ModelineSegment& segment : status->segments) {
                    output << "        " << modeline_group_name(segment.group)
                           << " tone=" << modeline_tone_name(segment.tone)
                           << " weight=" << modeline_weight_name(segment.weight)
                           << (segment.debug ? " debug" : "") << " text=\""
                           << printable(segment.text) << "\"\n";
                }
            }
            for (std::size_t index = 0; index < region.primitives().size(); ++index) {
                const ui::Prim& prim = region.primitives()[index];
                output << "      prim:" << (prim.id.empty() ? std::to_string(index) : prim.id)
                       << " cell=(" << prim.row << ',' << prim.col
                       << ") kind=" << prim_kind_name(prim.kind)
                       << " style=" << style_class_name(prim.style)
                       << (prim.selected ? " selected" : "") << " text=\"" << printable(prim.text)
                       << "\"\n";
            }
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
    if (frame.render.document_layout) {
        const DocumentLayoutSnapshot& document = *frame.render.document_layout;
        output << "    document-layout lines=" << document.lines.size();
        if (document.cursor_row && document.cursor_column) {
            output << " cursor=" << *document.cursor_row << ':' << *document.cursor_column
                   << " advance=" << document.cursor_advance
                   << " grid-x=" << document.grid_cursor_x;
        }
        output << '\n';
    }
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
           << " cursor-owner=" << frame.render.animation.cursor_owner
           << " scroll-progress=" << frame.render.animation.scroll_progress
           << " scroll-velocity=" << frame.render.animation.scroll_velocity
           << " cursor-progress=" << frame.render.animation.cursor_progress << '\n';
    if (frame.render.animation.scroll) {
        output << "      scroll-top=" << frame.render.animation.visual_scroll_top
               << " target=" << frame.render.animation.target_scroll_top << " layers=";
        for (std::size_t index = 0; index < frame.render.animation.layers.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            const RenderScrollLayerSnapshot& layer = frame.render.animation.layers[index];
            output << layer.scroll_top << '@' << layer.grid_offset_y << '[' << layer.clip_top << ','
                   << layer.clip_bottom << ')';
        }
        if (frame.render.animation.active_line_y) {
            output << " active-line-y=" << *frame.render.animation.active_line_y;
        }
        output << '\n';
    }
    output << "    damage=" << (frame.render.damage.full_repaint ? "full" : "partial")
           << " grid-transform=" << (frame.render.damage.grid_transform_changed ? "true" : "false")
           << " translation-rows=" << frame.render.damage.grid_translation_rows
           << " rects=" << frame.render.damage.rects.size()
           << " cells=" << frame.render.damage.damaged_cells
           << " output-pixels=" << frame.render.damage.damaged_output_pixels
           << " fraction=" << frame.render.damage.output_fraction
           << " reference-match=" << (frame.render.damage.full_reference_match ? "true" : "false")
           << '\n';
    if (frame.editor.interaction.active) {
        output << "      minibuffer window=" << frame.editor.interaction.window_slot << ':'
               << frame.editor.interaction.window_generation
               << " view=" << frame.editor.interaction.view_slot << ':'
               << frame.editor.interaction.view_generation
               << " buffer=" << frame.editor.interaction.buffer_slot << ':'
               << frame.editor.interaction.buffer_generation
               << " origin-window=" << frame.editor.interaction.origin_window_slot << ':'
               << frame.editor.interaction.origin_window_generation << '\n';
    }
    output << "    timings-us layout=" << frame.render.timings.layout_us
           << " compose=" << frame.render.timings.compose_us
           << " state=" << frame.render.timings.render_state_us
           << " inspect=" << frame.render.timings.inspect_us
           << " build=" << frame.render.timings.frame_build_us
           << " raster=" << frame.render.timings.raster_us
           << " reference=" << frame.render.timings.reference_us
           << " upload=" << frame.render.timings.upload_us
           << " present=" << frame.render.timings.present_us
           << " total=" << frame.render.timings.total_us
           << " uploaded-bytes=" << frame.render.timings.uploaded_bytes
           << " upload-rects=" << frame.render.timings.upload_rects
           << " texture-scroll=" << (frame.render.timings.texture_scroll_reused ? "true" : "false")
           << " copied-pixels=" << frame.render.timings.texture_copy_pixels
           << " shape-cache=" << frame.render.timings.shape_cache_hits << '/'
           << frame.render.timings.shape_cache_misses
           << " evictions=" << frame.render.timings.shape_cache_evictions
           << " entries=" << frame.render.timings.shape_cache_entries << '\n';
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
