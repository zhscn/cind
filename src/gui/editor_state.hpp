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
    std::string history;
    std::string provider;
    bool allow_custom_input = false;
    std::uint64_t generation = 0;
    std::size_t selected = 0;
    std::string error;
    std::vector<InteractionCandidateSnapshot> candidates;
};

struct OpenBufferStateSnapshot {
    std::uint32_t buffer_slot = 0;
    std::uint32_t buffer_generation = 0;
    std::uint32_t view_slot = 0;
    std::uint32_t view_generation = 0;
    std::string name;
    std::string resource;
    bool modified = false;
    bool active = false;
    bool saving = false;
};

struct CommandLoopStateSnapshot {
    std::vector<std::string> keymaps;
    std::string pending_keys;
    std::string pending_keymap;
    std::optional<std::int64_t> repeat_count;
    std::string last_command;
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
    CommandLoopStateSnapshot command_loop;
    InteractionStateSnapshot interaction;
    std::vector<OpenBufferStateSnapshot> buffers;
    bool quit_armed = false;
    bool quit = false;
};

} // namespace cind::gui
