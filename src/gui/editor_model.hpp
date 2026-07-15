#pragma once

#include "cli/session.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cstdint>
#include <string>
#include <string_view>

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

class EditorModel {
public:
    EditorModel(std::string path, std::string initial, CppIndentStyle style,
                std::string style_origin, std::uint32_t initial_line);

    ui::Scene compose(int rows, int columns);
    bool handle_key(SDL_Scancode scancode, SDL_Keymod modifiers, int page_rows);
    void insert_text(std::string_view text);
    void set_preedit(std::string_view text);
    void click(int cell_row, int cell_column);
    void set_frame_rows(int rows) { last_rows_ = rows; }
    void scroll_lines(int delta);
    bool should_quit() const { return quit_; }
    void request_quit(bool force = false);

    RevisionId revision() const { return session_.snapshot().revision(); }
    EditorStateSnapshot inspect();

private:
    bool dirty() const;
    const ui::LineSigns& signs();
    void after_edit();
    void save();
    void move_horizontal(bool forward);
    void move_vertical(int delta);
    void move_home();
    void move_end();
    void erase_code_point(bool forward);

    std::string path_;
    EditSession session_;
    std::string style_origin_;
    Text saved_text_;
    ui::EditorViewport viewport_;
    ui::LineSigns signs_;
    RevisionId sign_revision_ = static_cast<RevisionId>(-1);
    std::uint32_t save_generation_ = 0;
    std::uint32_t sign_generation_ = static_cast<std::uint32_t>(-1);
    int last_rows_ = 24;
    std::string message_;
    std::string preedit_;
    std::string last_key_;
    bool quit_armed_ = false;
    bool quit_ = false;
};

} // namespace cind::gui
