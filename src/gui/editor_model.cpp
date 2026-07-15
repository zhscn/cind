#include "gui/editor_model.hpp"

#include "commands/file_io.hpp"
#include "document/text.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <new>
#include <optional>
#include <system_error>
#include <utility>

namespace cind::gui {

namespace {

std::string_view key_name(EditorKey key) {
    switch (key) {
    case EditorKey::Unknown:
        return {};
    case EditorKey::A:
        return "A";
    case EditorKey::E:
        return "E";
    case EditorKey::N:
        return "N";
    case EditorKey::P:
        return "P";
    case EditorKey::Q:
        return "Q";
    case EditorKey::R:
        return "R";
    case EditorKey::S:
        return "S";
    case EditorKey::V:
        return "V";
    case EditorKey::Z:
        return "Z";
    case EditorKey::Left:
        return "Left";
    case EditorKey::Right:
        return "Right";
    case EditorKey::Up:
        return "Up";
    case EditorKey::Down:
        return "Down";
    case EditorKey::Home:
        return "Home";
    case EditorKey::End:
        return "End";
    case EditorKey::PageUp:
        return "PageUp";
    case EditorKey::PageDown:
        return "PageDown";
    case EditorKey::Backspace:
        return "Backspace";
    case EditorKey::Delete:
        return "Delete";
    case EditorKey::Enter:
        return "Enter";
    case EditorKey::Tab:
        return "Tab";
    }
    return {};
}

} // namespace

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
                                     .reveal_caret = reveal_caret_,
                                     .echo_cursor_column = std::nullopt},
                                    viewport_);
}

bool EditorModel::handle_key(EditorKey key, KeyModifiers modifiers, int page_rows) {
    const bool control = modifiers.control;
    const bool alt = modifiers.alt;
    const bool shift = modifiers.shift;
    bool handled = true;
    bool edited = false;

    if (const std::string_view name = key_name(key); !name.empty()) {
        last_key_ = name;
    }

    if (control) {
        switch (key) {
        case EditorKey::S:
            save();
            return true;
        case EditorKey::Q:
            request_quit(shift);
            return true;
        case EditorKey::Z:
            session_.undo();
            edited = true;
            break;
        case EditorKey::R:
            session_.redo();
            edited = true;
            break;
        case EditorKey::A:
            move_home();
            break;
        case EditorKey::E:
            move_end();
            break;
        case EditorKey::N:
            move_vertical(1);
            break;
        case EditorKey::P:
            move_vertical(-1);
            break;
        case EditorKey::V:
            move_vertical(page_rows);
            break;
        default:
            handled = false;
            break;
        }
    } else if (alt && key == EditorKey::V) {
        move_vertical(-page_rows);
    } else if (!alt) {
        switch (key) {
        case EditorKey::Left:
            move_horizontal(false);
            break;
        case EditorKey::Right:
            move_horizontal(true);
            break;
        case EditorKey::Up:
            move_vertical(-1);
            break;
        case EditorKey::Down:
            move_vertical(1);
            break;
        case EditorKey::Home:
            move_home();
            break;
        case EditorKey::End:
            move_end();
            break;
        case EditorKey::PageUp:
            move_vertical(-page_rows);
            break;
        case EditorKey::PageDown:
            move_vertical(page_rows);
            break;
        case EditorKey::Backspace:
            erase_grapheme(false);
            edited = true;
            break;
        case EditorKey::Delete:
            erase_grapheme(true);
            edited = true;
            break;
        case EditorKey::Enter:
            session_.enter();
            edited = true;
            break;
        case EditorKey::Tab:
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

void EditorModel::click(ui::CellPoint point) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const int text_column = ui::text_area_column(text.line_count());
    const int visible_rows = std::max(1, last_rows_ - 2);
    if (point.row < 0 || point.row >= visible_rows) {
        return;
    }
    const std::uint32_t line =
        std::min(viewport_.top_line + static_cast<std::uint32_t>(point.row), text.line_count() - 1);
    if (point.column < text_column) {
        session_.set_caret(text.line_start(line));
        reveal_caret_ = true;
        return;
    }
    const int display_column = viewport_.left_column + std::max(0, point.column - text_column);
    session_.set_caret(ui::offset_at_display_column(text, {.line = line, .column = display_column},
                                                    session_.style().tab_width));
    reveal_caret_ = true;
}

void EditorModel::scroll_lines(int delta) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const int last_line = static_cast<int>(snapshot.content().line_count()) - 1;
    viewport_.top_line = static_cast<std::uint32_t>(
        std::clamp(static_cast<int>(viewport_.top_line) + delta, 0, last_line));
    reveal_caret_ = false;
}

bool EditorModel::poll_background_work() {
    if (!pending_save_ ||
        pending_save_->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    std::error_code error;
    try {
        error = pending_save_->result.get();
    } catch (const std::system_error& exception) {
        error = exception.code();
    } catch (const std::bad_alloc&) {
        error = std::make_error_code(std::errc::not_enough_memory);
    } catch (...) {
        error = std::make_error_code(std::errc::io_error);
    }
    if (error) {
        message_ = std::format("save failed: {}", error.message());
    } else {
        saved_text_ = std::move(pending_save_->content);
        ++save_generation_;
        quit_armed_ = false;
        message_ = dirty() ? std::format("saved {} · newer edits remain", path_)
                           : std::format("saved {}", path_);
    }
    pending_save_.reset();
    return true;
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
    reveal_caret_ = true;
}

void EditorModel::save() {
    if (pending_save_) {
        message_ = "save already in progress";
        return;
    }
    const DocumentSnapshot snapshot = session_.snapshot();
    Text content = snapshot.content();
    std::string path = path_;
    try {
        pending_save_.emplace(PendingSave{
            content, std::async(std::launch::async,
                                [path = std::move(path), content = std::move(content)]() noexcept {
                                    try {
                                        return save_file_atomically(path, content);
                                    } catch (const std::system_error& exception) {
                                        return exception.code();
                                    } catch (const std::bad_alloc&) {
                                        return std::make_error_code(std::errc::not_enough_memory);
                                    } catch (...) {
                                        return std::make_error_code(std::errc::io_error);
                                    }
                                })});
        message_ = std::format("saving {}…", path_);
    } catch (const std::system_error& exception) {
        message_ = std::format("save failed: {}", exception.code().message());
    } catch (const std::bad_alloc&) {
        message_ = "save failed: not enough memory";
    }
}

void EditorModel::move_horizontal(bool forward) {
    const DocumentSnapshot snapshot = session_.snapshot();
    session_.set_caret(forward ? ui::next_grapheme(snapshot.content(), session_.caret())
                               : ui::previous_grapheme(snapshot.content(), session_.caret()));
    reveal_caret_ = true;
}

void EditorModel::move_vertical(int delta) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const LinePosition position = text.position(session_.caret());
    const int current_column =
        ui::display_column(text, session_.caret(), session_.style().tab_width);
    const int last_line = static_cast<int>(text.line_count()) - 1;
    const int target = std::clamp(static_cast<int>(position.line) + delta, 0, last_line);
    session_.set_caret(ui::offset_at_display_column(
        text, {.line = static_cast<std::uint32_t>(target), .column = current_column},
        session_.style().tab_width));
    reveal_caret_ = true;
}

void EditorModel::move_home() {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    session_.set_caret(text.line_start(text.position(session_.caret()).line));
    reveal_caret_ = true;
}

void EditorModel::move_end() {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    session_.set_caret(text.line_content_end(text.position(session_.caret()).line));
    reveal_caret_ = true;
}

void EditorModel::erase_grapheme(bool forward) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = session_.caret();
    if (forward) {
        session_.erase(TextRange{caret, ui::next_grapheme(text, caret)});
    } else {
        session_.erase(TextRange{ui::previous_grapheme(text, caret), caret});
    }
}

} // namespace cind::gui
