#include "gui/editor_model.hpp"

#include "commands/file_io.hpp"
#include "document/text.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/default_keymap.hpp"
#include "ui/char_width.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <new>
#include <optional>
#include <system_error>
#include <utility>

namespace cind::gui {

namespace {

BufferId create_file_buffer(EditorRuntime& runtime, std::string path, std::string initial) {
    const CppModeRegistration cpp = ensure_cpp_mode(runtime);
    const BufferId buffer = runtime.buffers().create(BufferSpec{.name = {},
                                                                .initial_text = std::move(initial),
                                                                .kind = BufferKind::File,
                                                                .resource_uri = std::move(path),
                                                                .read_only = false});
    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), cpp.mode);
    return buffer;
}

CommandResult completed() {
    return CommandCompleted{};
}

} // namespace

EditorModel::EditorModel(std::string path, std::string initial, CppIndentStyle style,
                         std::string style_origin, std::uint32_t initial_line)
    : buffer_id_(create_file_buffer(runtime_, std::move(path), std::move(initial))),
      view_id_(runtime_.views().create(buffer_id_)),
      session_(runtime_, buffer_id_, view_id_, style),
      basic_commands_(
          runtime_, session_,
          {.page_rows = [this] { return command_page_rows_; },
           .show_message = [this](std::string message) { message_ = std::move(message); },
           .edited = [this] { after_edit(); },
           .caret_moved = [this] { reveal_caret_ = true; }}),
      search_commands_(runtime_, session_,
                       [this](std::string message) {
                           message_ = std::move(message);
                           reveal_caret_ = true;
                       }),
      command_loop_(runtime_), style_origin_(std::move(style_origin)) {
    register_commands();
    keymap_ = runtime_.keymaps().define("editor.default");
    (void)bind_default_editor_keys(runtime_, keymap_);
    command_loop_.set_keymaps({keymap_});
    const DocumentSnapshot snapshot = session_.snapshot();
    if (initial_line > 0) {
        const std::uint32_t line = std::min(initial_line - 1, snapshot.content().line_count() - 1);
        session_.set_caret(snapshot.content().line_start(line));
    }
    message_ = "SDL3 Wayland · Skia · C-s save · C-q quit";
}

ui::Scene EditorModel::compose(int rows, int columns, float visible_text_rows) {
    const DocumentSnapshot snapshot = session_.snapshot();
    std::string minibuffer_echo;
    std::optional<int> echo_cursor;
    const std::string_view echo = [&]() -> std::string_view {
        if (const MinibufferState* minibuffer = command_loop_.minibuffer()) {
            minibuffer_echo = minibuffer->request.prompt + minibuffer->input;
            echo_cursor = ui::display_width(minibuffer_echo);
            return minibuffer_echo;
        }
        return preedit_.empty() ? std::string_view(message_) : std::string_view(preedit_);
    }();
    ViewportState& state = session_.view().viewport();
    ui::EditorViewport viewport{.top_line = state.top_line,
                                .top_line_offset = state.top_line_offset,
                                .left_column = state.left_column};
    ui::Scene scene = ui::compose_editor_scene({.text = snapshot.content(),
                                                .tokens = session_.analysis().tree.tokens(),
                                                .signs = signs(),
                                                .caret = session_.caret(),
                                                .selection = std::nullopt,
                                                .rows = rows,
                                                .cols = columns,
                                                .visible_text_rows = visible_text_rows,
                                                .tab_width = session_.style().tab_width,
                                                .path = path(),
                                                .dirty = dirty(),
                                                .revision = snapshot.revision(),
                                                .style_origin = style_origin_,
                                                .last_key = last_key_,
                                                .echo = echo,
                                                .reveal_caret = reveal_caret_,
                                                .echo_cursor_column = echo_cursor},
                                               viewport);
    state.top_line = viewport.top_line;
    state.top_line_offset = viewport.top_line_offset;
    state.left_column = viewport.left_column;
    return scene;
}

bool EditorModel::handle_key(KeyStroke key, int page_rows) {
    command_page_rows_ = std::max(1, page_rows);
    last_key_ = format_key_stroke(key);
    CommandContext context = command_context();

    if (command_loop_.minibuffer_active()) {
        if (key.code == KeyCode::Enter && key.modifiers == KeyModifier::None) {
            return handle_loop_result(command_loop_.submit_minibuffer(context));
        }
        if (key.code == KeyCode::Backspace && key.modifiers == KeyModifier::None) {
            (void)command_loop_.minibuffer_erase_backward();
            return true;
        }
        if ((key.code == KeyCode::Escape && key.modifiers == KeyModifier::None) ||
            key == KeyStroke::character_key(U'g', KeyModifier::Control)) {
            return handle_loop_result(command_loop_.cancel_minibuffer());
        }
        return true;
    }

    return handle_loop_result(command_loop_.dispatch(key, context));
}

void EditorModel::insert_text(std::string_view text) {
    if (text.empty()) {
        return;
    }
    if (command_loop_.minibuffer_active()) {
        command_loop_.minibuffer_insert(text);
        last_key_ = "text";
        preedit_.clear();
        return;
    }
    if (text.size() == 1 && static_cast<unsigned char>(text.front()) >= 0x20U) {
        session_.type_text(text);
    } else {
        session_.insert_text(text);
    }
    basic_commands_.reset_preferred_column();
    last_key_ = "text";
    after_edit();
}

void EditorModel::register_commands() {
    auto define = [this](std::string name, auto execute) {
        return runtime_.commands().define(
            std::move(name), [execute = std::move(execute)](
                                 CommandContext&, const CommandInvocation& invocation) mutable {
                execute(invocation);
                return completed();
            });
    };

    define("file.save", [this](const CommandInvocation&) { save(); });
    define("application.quit", [this](const CommandInvocation&) { request_quit(); });
    define("application.force-quit", [this](const CommandInvocation&) { request_quit(true); });
}

bool EditorModel::handle_loop_result(const CommandLoopResult& result) {
    if (result.command) {
        last_command_ = runtime_.commands().definition(*result.command).name;
    }
    if (result.status == CommandLoopStatus::Prefix || result.status == CommandLoopStatus::Error ||
        result.status == CommandLoopStatus::Disabled ||
        result.status == CommandLoopStatus::Cancelled ||
        (result.status == CommandLoopStatus::NotHandled && result.consumed)) {
        message_ = result.message;
    }
    return result.consumed;
}

void EditorModel::set_preedit(std::string_view text) {
    preedit_ = text.empty() ? std::string() : std::format("IME · {}", text);
}

void EditorModel::click(ui::CellPoint point) {
    basic_commands_.reset_preferred_column();
    const DocumentSnapshot snapshot = session_.snapshot();
    const Text& text = snapshot.content();
    const int text_column = ui::text_area_column(text.line_count());
    const int visible_rows = std::max(1, last_rows_ - 2);
    if (point.row < 0 || point.row >= visible_rows) {
        return;
    }
    const std::uint32_t line =
        std::min(session_.view().viewport().top_line + static_cast<std::uint32_t>(point.row),
                 text.line_count() - 1);
    if (point.column < text_column) {
        session_.set_caret(text.line_start(line));
        reveal_caret_ = true;
        return;
    }
    const int display_column =
        session_.view().viewport().left_column + std::max(0, point.column - text_column);
    session_.set_caret(ui::offset_at_display_column(text, {.line = line, .column = display_column},
                                                    session_.style().tab_width));
    reveal_caret_ = true;
}

void EditorModel::scroll_lines(int delta) {
    const DocumentSnapshot snapshot = session_.snapshot();
    const int last_line = static_cast<int>(snapshot.content().line_count()) - 1;
    ViewportState& viewport = session_.view().viewport();
    const double position = static_cast<double>(viewport.top_line) +
                            static_cast<double>(viewport.top_line_offset) +
                            static_cast<double>(delta);
    const double clamped = std::clamp(position, 0.0, static_cast<double>(last_line));
    const double integral = std::floor(clamped);
    viewport.top_line = static_cast<std::uint32_t>(integral);
    viewport.top_line_offset = static_cast<float>(clamped - integral);
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
        session_.buffer().mark_saved(std::move(pending_save_->content));
        ++save_generation_;
        quit_armed_ = false;
        message_ = dirty() ? std::format("saved {} · newer edits remain", path())
                           : std::format("saved {}", path());
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
    const ViewportState& view = session_.view().viewport();
    CommandLoopStateSnapshot command_state{.keymaps = {},
                                           .pending_keys = command_loop_.pending_sequence_text(),
                                           .pending_keymap = {},
                                           .repeat_count = command_loop_.repeat_count(),
                                           .last_command = last_command_,
                                           .minibuffer = {}};
    for (const KeymapId keymap : command_loop_.keymaps()) {
        command_state.keymaps.push_back(runtime_.keymaps().definition(keymap).name);
    }
    if (const std::optional<KeymapId> keymap = command_loop_.pending_keymap()) {
        command_state.pending_keymap = runtime_.keymaps().definition(*keymap).name;
    }
    if (const MinibufferState* minibuffer = command_loop_.minibuffer()) {
        command_state.minibuffer = {.active = true,
                                    .prompt = minibuffer->request.prompt,
                                    .input = minibuffer->input,
                                    .history = minibuffer->request.history,
                                    .completion_provider = minibuffer->request.completion_provider};
    }
    return {.path = path(),
            .revision = snapshot.revision(),
            .document_bytes = text.size_bytes(),
            .line_count = text.line_count(),
            .dirty = dirty(),
            .caret = caret,
            .caret_position = text.position(caret),
            .caret_display_column = ui::display_column(text, caret, session_.style().tab_width),
            .viewport = {.top_line = view.top_line,
                         .top_line_offset = view.top_line_offset,
                         .left_column = view.left_column},
            .line_signs = signs(),
            .tab_width = session_.style().tab_width,
            .style_origin = style_origin_,
            .message = message_,
            .preedit = preedit_,
            .last_key = last_key_,
            .command_loop = std::move(command_state),
            .quit_armed = quit_armed_,
            .quit = quit_};
}

bool EditorModel::dirty() const {
    return session_.buffer().modified();
}

const std::string& EditorModel::path() const {
    const std::optional<std::string>& resource = session_.buffer().resource_uri();
    if (!resource) {
        throw std::logic_error("file buffer has no resource URI");
    }
    return *resource;
}

const ui::LineSigns& EditorModel::signs() {
    const DocumentSnapshot snapshot = session_.snapshot();
    if (sign_revision_ != snapshot.revision() || sign_generation_ != save_generation_) {
        signs_ = ui::line_signs(session_.buffer().save_point(), snapshot.content());
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
    std::string target_path = path();
    try {
        pending_save_.emplace(PendingSave{
            content, std::async(std::launch::async, [path = std::move(target_path),
                                                     content = std::move(content)]() noexcept {
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
        message_ = std::format("saving {}…", path());
    } catch (const std::system_error& exception) {
        message_ = std::format("save failed: {}", exception.code().message());
    } catch (const std::bad_alloc&) {
        message_ = "save failed: not enough memory";
    }
}

} // namespace cind::gui
