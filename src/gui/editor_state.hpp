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
    std::uint32_t window_slot = 0;
    std::uint32_t window_generation = 0;
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    std::uint32_t view_slot = 0;
    std::uint32_t view_generation = 0;
    std::uint32_t origin_window_slot = 0;
    std::uint32_t origin_window_generation = 0;
    std::uint32_t origin_buffer_slot = 0;
    std::uint32_t origin_buffer_generation = 0;
    std::uint32_t origin_view_slot = 0;
    std::uint32_t origin_view_generation = 0;
    std::string kind;
    std::string keymap;
    std::string input_state;
    std::string buffer_name;
    std::string prompt;
    std::string input;
    std::size_t input_cursor = 0;
    std::string history;
    std::size_t history_entries = 0;
    std::optional<std::size_t> history_index = std::nullopt;
    std::string history_draft = {};
    std::string provider;
    bool allow_custom_input = false;
    std::uint64_t generation = 0;
    bool loading = false;
    std::size_t selected = 0;
    std::string error;
    std::vector<InteractionCandidateSnapshot> candidates;
};

struct CompletionItemStateSnapshot {
    std::uint64_t id = 0;
    std::string provider;
    std::string label;
    std::string kind;
    std::string detail;
    bool resolved = false;
    bool resolving = false;
    std::string resolve_error;
    std::string documentation;
};

struct CompletionStateSnapshot {
    bool active = false;
    std::uint64_t generation = 0;
    RevisionId revision = 0;
    TextOffset anchor;
    TextOffset caret;
    std::string query;
    std::size_t selected = 0;
    std::vector<std::string> pending_providers;
    std::vector<CompletionItemStateSnapshot> items;
};

struct LspSessionStateSnapshot {
    std::uint64_t id = 0;
    std::string state;
    std::string command;
    std::string root;
    std::size_t pending_requests = 0;
    std::size_t open_documents = 0;
    std::string server_capabilities = "{}";
    std::string error;
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
    bool completion_auto = false;
    std::vector<ModeThingStateSnapshot> things;
    std::vector<std::string> completion_providers;
    std::size_t location_count = 0;
    std::size_t diagnostic_count = 0;
    std::size_t diagnostic_errors = 0;
    std::size_t diagnostic_warnings = 0;
};

struct OpenWindowStateSnapshot {
    std::uint32_t window_slot = 0;
    std::uint32_t window_generation = 0;
    std::uint32_t view_slot = 0;
    std::uint32_t view_generation = 0;
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    std::string role;
    bool pinned = false;
    bool created_by_policy = false;
    bool active = false;
    std::vector<std::string> input_states;
};

struct EntityStateSnapshot {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const EntityStateSnapshot&, const EntityStateSnapshot&) = default;
};

struct WorkbenchSlotStateSnapshot {
    std::string role;
    EntityStateSnapshot window;
};

struct WorkbenchLayoutStateSnapshot {
    bool leaf = false;
    EntityStateSnapshot window;
    std::string axis;
    float ratio = 0.5F;
    std::vector<WorkbenchLayoutStateSnapshot> children;
};

struct WorkbenchStateSnapshot {
    EntityStateSnapshot workbench;
    std::string name;
    bool active = false;
    std::vector<EntityStateSnapshot> scope;
    std::vector<EntityStateSnapshot> mru;
    EntityStateSnapshot active_window;
    std::vector<WorkbenchSlotStateSnapshot> slots;
    WorkbenchLayoutStateSnapshot layout;
    std::vector<OpenWindowStateSnapshot> windows;
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
    EncodedLinePosition target;
};

struct LocationNavigationStateSnapshot {
    bool present = false;
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    std::optional<std::size_t> selected_index;
    std::size_t location_count = 0;
};

struct JumpNodeStateSnapshot {
    std::uint64_t id = 0;
    bool attached = false;
    EntityStateSnapshot buffer;
    std::uint64_t anchor = 0;
    std::string resource;
    LinePosition fallback;
    std::string excerpt;
    std::uint64_t created_at = 0;
    std::uint64_t last_visit = 0;
};

struct JumpEdgeStateSnapshot {
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    std::string kind;
    std::uint64_t at = 0;
    bool persistent = false;
};

struct JumpWalkStateSnapshot {
    EntityStateSnapshot window;
    std::vector<std::uint64_t> entries;
    std::optional<std::size_t> cursor;
};

struct WorkbenchJumpStateSnapshot {
    EntityStateSnapshot workbench;
    std::vector<JumpNodeStateSnapshot> nodes;
    std::vector<JumpEdgeStateSnapshot> edges;
    std::vector<JumpWalkStateSnapshot> walks;
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
    bool reveal_caret = true;
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
    std::string text_input_command;
    bool text_input_command_available = false;
    std::string selection_after_edit;
    bool input_state_handler = false;
    bool input_state_on_enter = false;
    bool input_state_on_exit = false;
    PositionHintsStateSnapshot position_hints;
    CommandLoopStateSnapshot command_loop;
    ScriptingStateSnapshot scripting;
    InteractionStateSnapshot interaction;
    CompletionStateSnapshot completion;
    std::vector<LspSessionStateSnapshot> lsp;
    std::vector<OpenBufferStateSnapshot> buffers;
    std::vector<OpenWindowStateSnapshot> windows;
    std::vector<WorkbenchStateSnapshot> workbenches;
    std::vector<ProjectStateSnapshot> projects;
    LocationStateSnapshot location_at_caret;
    LocationNavigationStateSnapshot location_navigation;
    std::vector<WorkbenchJumpStateSnapshot> jumps;
    bool background_work = false;
    bool project_search_running = false;
    bool quit = false;
};

} // namespace cind::gui
