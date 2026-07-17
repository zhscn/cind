#pragma once

#include "document/document.hpp"
#include "editor/command.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cind::gui {

struct ModeThingStateSnapshot {
    std::string name;
    std::string definition;
};

struct InteractionCandidateSnapshot {
    std::string value;
    std::string label;
    std::string detail;
};

struct InteractionStateSnapshot {
    bool active = false;
    std::string kind;
    std::string prompt;
    std::string input;
    std::size_t input_cursor = 0;
    std::string history;
    std::string provider;
    bool allow_custom_input = false;
    std::uint64_t generation = 0;
    bool loading = false;
    std::size_t selected = 0;
    std::string error;
    std::vector<InteractionCandidateSnapshot> candidates;
};

struct OpenBufferStateSnapshot {
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    bool view_present = false;
    std::uint32_t view_slot = 0;
    std::uint32_t view_generation = 0;
    std::string name;
    std::string resource;
    bool modified = false;
    bool active = false;
    bool saving = false;
    std::string major_mode;
    std::string interaction_class;
    std::string initial_input_state;
    std::vector<ModeThingStateSnapshot> things;
    std::size_t location_count = 0;
};

struct OpenWindowStateSnapshot {
    std::uint32_t window_slot = 0;
    std::uint32_t window_generation = 0;
    std::uint32_t view_slot = 0;
    std::uint32_t view_generation = 0;
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    bool active = false;
    std::vector<std::string> input_states;
};

struct ProjectStateSnapshot {
    std::uint32_t project_slot = 0;
    std::uint32_t project_generation = 0;
    std::string name;
    std::vector<std::string> roots;
    std::string discovery_provider;
    std::string discovery_marker;
    std::size_t file_count = 0;
    std::uint64_t index_revision = 0;
    bool indexing = false;
    std::string index_error;
};

struct LocationStateSnapshot {
    bool present = false;
    TextRange source_range;
    std::string resource;
    LinePosition target;
};

struct LocationNavigationStateSnapshot {
    bool present = false;
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    std::optional<std::size_t> selected_index;
    std::size_t location_count = 0;
};

struct KeymapLayerStateSnapshot {
    std::string name;
    std::string scope;
    std::vector<std::string> parents;
};

struct CommandLoopStateSnapshot {
    std::vector<std::string> keymaps;
    std::vector<KeymapLayerStateSnapshot> layers;
    std::vector<std::string> override_keymaps;
    std::string pending_keys;
    std::string pending_keymap;
    std::string pending_input_state;
    std::optional<std::int64_t> repeat_count;
    std::optional<std::string> register_name;
    std::vector<CommandPrefixExtra> prefix_extra;
    std::string prefix_text;
    std::string last_command;
};

struct SelectionRangeStateSnapshot {
    TextOffset anchor;
    TextOffset head;
    std::string granularity;
};

struct SelectionStateSnapshot {
    bool active = false;
    std::size_t primary = 0;
    std::size_t history_depth = 0;
    std::string metadata;
    std::vector<SelectionRangeStateSnapshot> ranges;
};

struct PositionHintStateSnapshot {
    TextOffset position;
    std::string label;
};

struct PositionHintsStateSnapshot {
    bool provider = false;
    std::vector<PositionHintStateSnapshot> items;
    std::optional<std::string> error;
};

struct ScriptingStateSnapshot {
    std::string engine;
    std::string version;
    std::vector<std::string> modules;
    std::vector<std::string> extensions;
    std::uint64_t command_revision = 0;
    std::size_t scripted_commands = 0;
    std::uint64_t provider_revision = 0;
    std::size_t scripted_providers = 0;
    std::uint64_t binding_revision = 0;
    std::uint64_t input_state_revision = 0;
    std::size_t scripted_input_states = 0;
    std::size_t scripted_input_strategies = 0;
    std::uint64_t mode_revision = 0;
    std::size_t scripted_modes = 0;
    std::uint64_t resource_policy_revision = 0;
    std::size_t scripted_file_mode_rules = 0;
    std::size_t scripted_project_providers = 0;
    std::size_t outstanding_async_tasks = 0;
    std::optional<std::string> last_error;
};

struct EditorRenderState {
    RevisionId revision = 0;
    ui::EditorViewport viewport;
    std::uint32_t window_slot = 0;
    std::uint32_t window_generation = 0;
    std::uint32_t view_slot = 0;
    std::uint32_t view_generation = 0;
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
};

struct EditorStateSnapshot {
    std::string path;
    RevisionId revision = 0;
    std::uint32_t document_bytes = 0;
    std::uint32_t line_count = 0;
    bool dirty = false;
    TextOffset caret;
    LinePosition caret_position;
    int caret_display_column = 0;
    SelectionStateSnapshot selection;
    ui::EditorViewport viewport;
    ui::LineSigns line_signs;
    int tab_width = 4;
    std::string style_origin;
    std::string message;
    std::string preedit;
    std::string last_key;
    std::uint32_t active_window_slot = 0;
    std::uint32_t active_window_generation = 0;
    std::string input_focus;
    std::string input_strategy;
    std::string input_state;
    std::string input_cursor_shape;
    std::string input_state_indicator;
    std::string text_input_policy;
    std::string selection_after_edit;
    bool input_state_handler = false;
    bool input_state_on_enter = false;
    bool input_state_on_exit = false;
    PositionHintsStateSnapshot position_hints;
    CommandLoopStateSnapshot command_loop;
    ScriptingStateSnapshot scripting;
    InteractionStateSnapshot interaction;
    std::vector<OpenBufferStateSnapshot> buffers;
    std::vector<OpenWindowStateSnapshot> windows;
    std::vector<ProjectStateSnapshot> projects;
    LocationStateSnapshot location_at_caret;
    LocationNavigationStateSnapshot location_navigation;
    bool background_work = false;
    bool project_search_running = false;
    bool quit_armed = false;
    bool quit = false;
};

} // namespace cind::gui
