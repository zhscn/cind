#include "editor/editor_application.hpp"

#include "commands/file_io.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/default_keymap.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <new>
#include <stdexcept>
#include <utility>

namespace cind {

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

EditorApplication::EditorApplication(EditorApplicationSpec spec)
    : buffer_id_(create_file_buffer(runtime_, std::move(spec.path), std::move(spec.initial_text))),
      view_id_(runtime_.views().create(buffer_id_)),
      session_(runtime_, buffer_id_, view_id_, spec.style),
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
      command_loop_(runtime_), style_origin_(std::move(spec.style_origin)) {
    register_commands();
    keymap_ = runtime_.keymaps().define("editor.default");
    refresh_default_keymap();
    command_loop_.set_keymaps({keymap_});

    const DocumentSnapshot snapshot = session_.snapshot();
    if (spec.initial_line > 0) {
        const std::uint32_t line =
            std::min(spec.initial_line - 1, snapshot.content().line_count() - 1);
        session_.set_caret(snapshot.content().line_start(line));
    }
}

void EditorApplication::refresh_default_keymap() {
    (void)bind_default_editor_keys(runtime_, keymap_);
}

bool EditorApplication::handle_key(KeyStroke key, int page_rows) {
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

void EditorApplication::insert_text(std::string_view text) {
    if (text.empty()) {
        return;
    }
    if (command_loop_.minibuffer_active()) {
        command_loop_.minibuffer_insert(text);
        last_key_ = "text";
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

const std::string& EditorApplication::path() const {
    const std::optional<std::string>& resource = session_.buffer().resource_uri();
    if (!resource) {
        throw std::logic_error("file buffer has no resource URI");
    }
    return *resource;
}

bool EditorApplication::poll_background_work() {
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
        mark_saved(std::move(pending_save_->content));
        message_ = dirty() ? std::format("saved {} · newer edits remain", path())
                           : std::format("saved {}", path());
    }
    pending_save_.reset();
    return true;
}

void EditorApplication::request_quit(bool force) {
    if (!dirty() || force || quit_armed_) {
        quit_ = true;
        return;
    }
    quit_armed_ = true;
    message_ = "unsaved changes · Ctrl-S saves · Ctrl-Q again or Ctrl-Shift-Q discards";
}

void EditorApplication::mark_saved(Text content) {
    session_.buffer().mark_saved(std::move(content));
    ++save_generation_;
    quit_armed_ = false;
}

void EditorApplication::register_commands() {
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

bool EditorApplication::handle_loop_result(const CommandLoopResult& result) {
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

void EditorApplication::after_edit() {
    quit_armed_ = false;
    message_.clear();
    reveal_caret_ = true;
}

void EditorApplication::save() {
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

} // namespace cind
