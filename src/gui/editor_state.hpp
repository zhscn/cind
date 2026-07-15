#pragma once

#include "document/document.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cind::gui {

struct MinibufferStateSnapshot {
    bool active = false;
    std::string prompt;
    std::string input;
    std::string history;
    std::string completion_provider;
};

struct CommandLoopStateSnapshot {
    std::vector<std::string> keymaps;
    std::string pending_keys;
    std::string pending_keymap;
    std::optional<std::int64_t> repeat_count;
    std::string last_command;
    MinibufferStateSnapshot minibuffer;
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
    bool quit_armed = false;
    bool quit = false;
};

} // namespace cind::gui
