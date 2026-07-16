#pragma once

#include "document/document.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cind::gui {

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
};

struct ProjectStateSnapshot {
    std::uint32_t project_slot = 0;
    std::uint32_t project_generation = 0;
    std::string name;
    std::vector<std::string> roots;
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
    std::optional<std::int64_t> repeat_count;
    std::string last_command;
};

struct ScriptingStateSnapshot {
    std::string engine;
    std::string version;
    std::vector<std::string> modules;
    std::uint64_t binding_revision = 0;
    std::optional<std::string> last_error;
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
