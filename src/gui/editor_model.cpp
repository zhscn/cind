#include "gui/editor_model.hpp"

#include "commands/file_io.hpp"
#include "document/text.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace cind::gui {

EditorModel::EditorModel(std::string path, std::string initial, CppIndentStyle style,
                         std::string style_origin, std::uint32_t initial_line)
    : path_(std::move(path)), session_(std::move(initial), style),
      style_origin_(std::move(style_origin)), saved_text_(session_.snapshot().content()) {
    const DocumentSnapshot snapshot = session_.snapshot();
    if (initial_line > 0) {
        const std::uint32_t line = std::min(initial_line - 1, snapshot.content().line_count() - 1);
        session_.set_caret(snapshot.content().line_start(line));
    }
    message_ = "SDL3 Wayland · Skia · Ctrl-S save · Ctrl-Q quit";
}

ui::Scene EditorModel::compose(int rows, int columns) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const std::string_view echo =
        preedit_.empty() ? std::string_view(message_) : std::string_view(preedit_);
    return ui::compose_editor_scene({.text = snapshot.content(),
                                     .tokens = session_.analysis().tree.tokens(),
                                     .signs = signs(),
                                     .caret = session_.caret(),
                                     .selection = std::nullopt,
                                     .rows = rows,
                                     .cols = columns,
                                     .tab_width = session_.style().tab_width,
                                     .path = path_,
                                     .dirty = dirty(),
                                     .revision = snapshot.revision(),
                                     .style_origin = style_origin_,
                                     .last_key = last_key_,
                                     .echo = echo,
                                     .echo_cursor_column = std::nullopt},
                                    viewport_);
}

bool EditorModel::handle_key(SDL_Scancode scancode, SDL_Keymod modifiers, int page_rows) {
    const bool control = (modifiers & SDL_KMOD_CTRL) != 0;
    const bool alt = (modifiers & SDL_KMOD_ALT) != 0;
    const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;
    bool handled = true;
    bool edited = false;

    if (const char* name = SDL_GetScancodeName(scancode); name && *name) {
        last_key_ = name;
    }

    if (control) {
        switch (scancode) {
        case SDL_SCANCODE_S:
            save();
            return true;
        case SDL_SCANCODE_Q:
            request_quit(shift);
            return true;
        case SDL_SCANCODE_Z:
            session_.undo();
            edited = true;
            break;
        case SDL_SCANCODE_R:
            session_.redo();
            edited = true;
            break;
        case SDL_SCANCODE_A:
            move_home();
            break;
        case SDL_SCANCODE_E:
            move_end();
            break;
        case SDL_SCANCODE_N:
            move_vertical(1);
            break;
        case SDL_SCANCODE_P:
            move_vertical(-1);
            break;
        case SDL_SCANCODE_V:
            move_vertical(page_rows);
            break;
        default:
            handled = false;
            break;
        }
    } else if (alt && scancode == SDL_SCANCODE_V) {
        move_vertical(-page_rows);
    } else if (!alt) {
        switch (scancode) {
        case SDL_SCANCODE_LEFT:
            move_horizontal(false);
            break;
        case SDL_SCANCODE_RIGHT:
            move_horizontal(true);
            break;
        case SDL_SCANCODE_UP:
            move_vertical(-1);
            break;
        case SDL_SCANCODE_DOWN:
            move_vertical(1);
            break;
        case SDL_SCANCODE_HOME:
            move_home();
            break;
        case SDL_SCANCODE_END:
            move_end();
            break;
        case SDL_SCANCODE_PAGEUP:
            move_vertical(-page_rows);
            break;
        case SDL_SCANCODE_PAGEDOWN:
            move_vertical(page_rows);
            break;
        case SDL_SCANCODE_BACKSPACE:
            erase_code_point(false);
            edited = true;
            break;
        case SDL_SCANCODE_DELETE:
            erase_code_point(true);
            edited = true;
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            session_.enter();
            edited = true;
            break;
        case SDL_SCANCODE_TAB:
            session_.indent();
            edited = true;
            break;
        default:
            handled = false;
            break;
        }
    } else {
        handled = false;
    }

    if (handled && edited) {
        after_edit();
    }
    return handled;
}

void EditorModel::insert_text(std::string_view text) {
    if (text.empty()) {
        return;
    }
    if (text.size() == 1 && static_cast<unsigned char>(text.front()) >= 0x20U) {
        session_.type_text(text);
    } else {
        session_.insert_text(text);
    }
    last_key_ = "text";
    after_edit();
}

void EditorModel::set_preedit(std::string_view text) {
    preedit_ = text.empty() ? std::string() : std::format("IME · {}", text);
}

void EditorModel::click(int cell_row, int cell_column) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const int text_column = ui::text_area_column(text.line_count());
    const int visible_rows = std::max(1, last_rows_ - 2);
    if (cell_row < 0 || cell_row >= visible_rows) {
        return;
    }
    const std::uint32_t line =
        std::min(viewport_.top_line + static_cast<std::uint32_t>(cell_row), text.line_count() - 1);
    if (cell_column < text_column) {
        session_.set_caret(text.line_start(line));
        return;
    }
    const int display_column = viewport_.left_column + std::max(0, cell_column - text_column);
    session_.set_caret(
        ui::offset_at_display_column(text, line, display_column, session_.style().tab_width));
}

void EditorModel::scroll_lines(int delta) {
    move_vertical(delta);
}

void EditorModel::request_quit(bool force) {
    if (!dirty() || force || quit_armed_) {
        quit_ = true;
        return;
    }
    quit_armed_ = true;
    message_ = "unsaved changes · Ctrl-S saves · Ctrl-Q again or Ctrl-Shift-Q discards";
}

EditorStateSnapshot EditorModel::inspect() {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = session_.caret();
    return {.path = path_,
            .revision = snapshot.revision(),
            .document_bytes = text.size_bytes(),
            .line_count = text.line_count(),
            .dirty = dirty(),
            .caret = caret,
            .caret_position = text.position(caret),
            .caret_display_column = ui::display_column(text, caret, session_.style().tab_width),
            .viewport = viewport_,
            .line_signs = signs(),
            .tab_width = session_.style().tab_width,
            .style_origin = style_origin_,
            .message = message_,
            .preedit = preedit_,
            .last_key = last_key_,
            .quit_armed = quit_armed_,
            .quit = quit_};
}

bool EditorModel::dirty() const {
    return diff_edit(saved_text_, session_.snapshot().content()).has_value();
}

const ui::LineSigns& EditorModel::signs() {
    const DocumentSnapshot snapshot = session_.snapshot();
    if (sign_revision_ != snapshot.revision() || sign_generation_ != save_generation_) {
        signs_ = ui::line_signs(saved_text_, snapshot.content());
        sign_revision_ = snapshot.revision();
        sign_generation_ = save_generation_;
    }
    return signs_;
}

void EditorModel::after_edit() {
    quit_armed_ = false;
    message_.clear();
    preedit_.clear();
}

void EditorModel::save() {
    const DocumentSnapshot snapshot = session_.snapshot();
    if (std::error_code error = save_file_atomically(path_, snapshot.content())) {
        message_ = std::format("save failed: {}", error.message());
        return;
    }
    saved_text_ = snapshot.content();
    ++save_generation_;
    quit_armed_ = false;
    message_ = std::format("saved {}", path_);
}

void EditorModel::move_horizontal(bool forward) {
    const DocumentSnapshot snapshot = session_.snapshot();
    session_.set_caret(forward ? ui::next_code_point(snapshot.content(), session_.caret())
                               : ui::previous_code_point(snapshot.content(), session_.caret()));
}

void EditorModel::move_vertical(int delta) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const LinePosition position = text.position(session_.caret());
    const int current_column =
        ui::display_column(text, session_.caret(), session_.style().tab_width);
    const int last_line = static_cast<int>(text.line_count()) - 1;
    const int target = std::clamp(static_cast<int>(position.line) + delta, 0, last_line);
    session_.set_caret(ui::offset_at_display_column(text, static_cast<std::uint32_t>(target),
                                                    current_column, session_.style().tab_width));
}

void EditorModel::move_home() {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    session_.set_caret(text.line_start(text.position(session_.caret()).line));
}

void EditorModel::move_end() {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    session_.set_caret(text.line_content_end(text.position(session_.caret()).line));
}

void EditorModel::erase_code_point(bool forward) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = session_.caret();
    if (forward) {
        session_.erase(TextRange{caret, ui::next_code_point(text, caret)});
    } else {
        session_.erase(TextRange{ui::previous_code_point(text, caret), caret});
    }
}

} // namespace cind::gui
