#pragma once

#include "document/document.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"

#include <cstdint>
#include <string>

namespace cind::gui {

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
    bool quit_armed = false;
    bool quit = false;
};

} // namespace cind::gui
