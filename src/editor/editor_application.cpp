#include "editor/editor_application.hpp"

#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/default_keymap.hpp"
#include "syntax/structure.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

namespace fs = std::filesystem;

CommandResult completed() {
    return CommandCompleted{};
}

const std::string* submitted_string(const CommandInvocation& invocation) {
    if (invocation.arguments.empty()) {
        return nullptr;
    }
    return std::get_if<std::string>(&invocation.arguments.back());
}

std::expected<std::string, std::string> normalized_path(std::string_view input) {
    if (input.empty()) {
        return std::unexpected("file path is empty");
    }
    std::error_code error;
    fs::path path = fs::absolute(fs::path(input), error).lexically_normal();
    if (error) {
        return std::unexpected(std::format("invalid path: {}", error.message()));
    }
    return path.string();
}

std::string directory_input(const fs::path& path) {
    std::string result = path.lexically_normal().string();
    if (result.empty() || result.back() != fs::path::preferred_separator) {
        result.push_back(fs::path::preferred_separator);
    }
    return result;
}

bool internal_command(std::string_view name) {
    return name.ends_with(".accept") || name.starts_with("interaction.");
}

} // namespace

EditorApplication::EditorApplication(EditorApplicationSpec spec)
    : interaction_(runtime_.interaction_providers()),
      basic_commands_(
          runtime_, [this](ViewId view) -> EditSession& { return session_for(view); },
          {.page_rows = [this] { return command_page_rows_; },
           .show_message = [this](std::string message) { message_ = std::move(message); },
           .edited = [this] { after_edit(); },
           .caret_moved = [this] { reveal_caret_ = true; }}),
      search_commands_(
          runtime_, [this](ViewId view) -> EditSession& { return session_for(view); },
          [this](std::string message) {
              message_ = std::move(message);
              reveal_caret_ = true;
          }),
      command_loop_(runtime_), platform_services_(std::move(spec.platform_services)) {
    register_commands();
    register_interaction_providers();

    BufferSpec initial_buffer{.name = {},
                              .initial_text = std::move(spec.initial_text),
                              .kind = spec.path.empty() ? BufferKind::Scratch : BufferKind::File,
                              .resource_uri = std::nullopt,
                              .read_only = false};
    if (!spec.path.empty()) {
        std::expected<std::string, std::string> path = normalized_path(spec.path);
        if (!path) {
            throw std::invalid_argument(path.error());
        }
        initial_buffer.resource_uri = std::move(*path);
    }
    const BufferId initial =
        create_buffer(std::move(initial_buffer), spec.style, std::move(spec.style_origin));
    const ViewId initial_view = create_view({}, initial);
    active_window_ = runtime_.windows().create(initial_view);
    view_state_for(initial_view).window = active_window_;
    if (spec.initial_line > 0) {
        const DocumentSnapshot snapshot = session().snapshot();
        const std::uint32_t line =
            std::min(spec.initial_line - 1, snapshot.content().line_count() - 1);
        session().set_caret(snapshot.content().line_start(line));
    }

    register_keymaps();
    sync_keymaps();
}

BufferId EditorApplication::buffer_id() const {
    return runtime_.views().get(view_id()).buffer_id();
}

ViewId EditorApplication::view_id() const {
    return runtime_.windows().get(active_window_).view_id();
}

EditSession& EditorApplication::session() {
    return *active_view().session;
}

const EditSession& EditorApplication::session() const {
    return *active_view().session;
}

void EditorApplication::refresh_default_keymap() {
    (void)bind_default_editor_keys(runtime_, keymap_);
}

bool EditorApplication::handle_key(KeyStroke key, int page_rows) {
    command_page_rows_ = std::max(1, page_rows);
    last_key_ = format_key_stroke(key);
    sync_keymaps();
    CommandContext context = command_context();
    const bool interaction_focus = interaction_.active();
    const bool consumed = handle_loop_result(command_loop_.dispatch(key, context));
    sync_keymaps();
    // An interaction is a capturing input focus. Unbound modified keys do
    // not fall through to the window, while printable text still arrives on
    // the frontend's text-input channel.
    return consumed || interaction_focus;
}

void EditorApplication::insert_text(std::string_view text) {
    if (text.empty()) {
        return;
    }
    if (interaction_.active()) {
        CommandContext context = command_context();
        interaction_.insert(text, context);
        last_key_ = "text";
        return;
    }
    if (text.size() == 1 && static_cast<unsigned char>(text.front()) >= 0x20U) {
        session().type_text(text);
    } else {
        session().insert_text(text);
    }
    reset_preferred_column();
    last_key_ = "text";
    after_edit();
}

void EditorApplication::reset_preferred_column() {
    basic_commands_.reset_preferred_column(view_id());
}

std::expected<BufferId, std::string> EditorApplication::open_file(std::string_view input) {
    std::expected<std::string, std::string> normalized = normalized_path(input);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    if (const std::optional<BufferId> existing = runtime_.buffers().find_by_resource(*normalized)) {
        (void)switch_buffer(*existing);
        return *existing;
    }

    const fs::path path(*normalized);
    std::error_code error;
    if (fs::is_directory(path, error)) {
        return std::unexpected("path names a directory");
    }
    std::string initial;
    if (fs::exists(path, error)) {
        std::ifstream input_file(path, std::ios::binary);
        if (!input_file) {
            return std::unexpected(std::format("cannot open {}", path.string()));
        }
        initial.assign(std::istreambuf_iterator<char>(input_file),
                       std::istreambuf_iterator<char>());
    } else if (error) {
        return std::unexpected(
            std::format("cannot inspect {}: {}", path.string(), error.message()));
    }

    CppIndentStyle style;
    std::string origin = "llvm (fallback)";
    if (std::optional<LoadedStyle> loaded = load_clang_format_style(path.parent_path())) {
        style = loaded->style;
        origin = loaded->config_path.filename().string();
    }
    try {
        const BufferId buffer = create_buffer(BufferSpec{.name = {},
                                                         .initial_text = std::move(initial),
                                                         .kind = BufferKind::File,
                                                         .resource_uri = *normalized,
                                                         .read_only = false},
                                              style, std::move(origin));
        (void)switch_buffer(buffer);
        message_ = std::format("opened {}", path.string());
        return buffer;
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

bool EditorApplication::switch_buffer(BufferId buffer) {
    return show_buffer(active_window_, buffer);
}

std::expected<void, std::string> EditorApplication::kill_buffer(BufferId buffer, bool force) {
    auto found =
        std::ranges::find_if(buffers_, [buffer](const std::unique_ptr<BufferState>& state) {
            return state->buffer == buffer;
        });
    if (found == buffers_.end()) {
        return std::unexpected("unknown buffer");
    }
    BufferState& target = **found;
    if (target.pending_save) {
        return std::unexpected("buffer has a save in progress");
    }
    if (runtime_.buffers().get(buffer).modified() && !force) {
        return std::unexpected("buffer has unsaved changes");
    }

    if (buffers_.size() == 1) {
        (void)create_scratch_buffer();
        found = std::ranges::find_if(
            buffers_, [buffer](const auto& state) { return state->buffer == buffer; });
    }
    const BufferId replacement = (*std::ranges::find_if(buffers_, [buffer](const auto& state) {
                                     return state->buffer != buffer;
                                 }))->buffer;
    std::vector<WindowId> affected_windows;
    for (const std::unique_ptr<ViewState>& view : views_) {
        if (view->buffer == buffer) {
            affected_windows.push_back(view->window);
        }
    }
    std::ranges::sort(affected_windows);
    const auto unique_end = std::ranges::unique(affected_windows).begin();
    affected_windows.erase(unique_end, affected_windows.end());
    for (const WindowId window : affected_windows) {
        if (window && runtime_.windows().try_get(window) != nullptr &&
            runtime_.views().get(runtime_.windows().get(window).view_id()).buffer_id() == buffer) {
            (void)show_buffer(window, replacement);
        }
    }

    for (auto it = views_.begin(); it != views_.end();) {
        if ((*it)->buffer != buffer) {
            ++it;
            continue;
        }
        const ViewId view = (*it)->view;
        it = views_.erase(it);
        if (!runtime_.views().erase(view)) {
            throw std::logic_error("view lifecycle registry is inconsistent");
        }
    }
    buffers_.erase(found);
    if (!runtime_.buffers().erase(buffer)) {
        throw std::logic_error("buffer lifecycle registry is inconsistent");
    }
    reveal_caret_ = true;
    quit_armed_ = false;
    sync_keymaps();
    return {};
}

std::vector<OpenBufferSnapshot> EditorApplication::open_buffers() const {
    std::vector<OpenBufferSnapshot> result;
    result.reserve(buffers_.size());
    for (const std::unique_ptr<BufferState>& entry : buffers_) {
        const BufferState& state = *entry;
        const Buffer& buffer = runtime_.buffers().get(state.buffer);
        const ViewState* view = find_view(active_window_, state.buffer);
        result.push_back({.buffer = state.buffer,
                          .view = view != nullptr ? std::optional(view->view) : std::nullopt,
                          .name = buffer.name(),
                          .resource = buffer.resource_uri(),
                          .modified = buffer.modified(),
                          .active = state.buffer == buffer_id(),
                          .saving = state.pending_save.has_value()});
    }
    return result;
}

std::vector<OpenWindowSnapshot> EditorApplication::open_windows() const {
    std::vector<OpenWindowSnapshot> result;
    for (const WindowId window : runtime_.windows().all()) {
        const ViewId view = runtime_.windows().get(window).view_id();
        result.push_back({.window = window,
                          .view = view,
                          .buffer = runtime_.views().get(view).buffer_id(),
                          .active = window == active_window_});
    }
    return result;
}

std::vector<KeyBindingHint> EditorApplication::pending_key_hints() const {
    std::vector<KeyBindingHint> result;
    for (const KeymapCompletion& completion : command_loop_.pending_completions()) {
        std::string command;
        if (completion.command) {
            command = runtime_.commands().definition(*completion.command).name;
        }
        result.push_back({.key = format_key_stroke(completion.key),
                          .command = std::move(command),
                          .prefix = completion.prefix});
    }
    return result;
}

const std::string& EditorApplication::path() const {
    const Buffer& buffer = session().buffer();
    return buffer.resource_uri() ? *buffer.resource_uri() : buffer.name();
}

const std::string& EditorApplication::style_origin() const {
    return active_buffer().style_origin;
}

std::uint32_t EditorApplication::save_generation() const {
    return active_buffer().save_generation;
}

bool EditorApplication::has_background_work() const {
    return std::ranges::any_of(buffers_, [](const std::unique_ptr<BufferState>& state) {
        return state->pending_save.has_value();
    });
}

bool EditorApplication::poll_background_work() {
    bool changed = false;
    for (const std::unique_ptr<BufferState>& state : buffers_) {
        if (!state->pending_save || state->pending_save->result.wait_for(std::chrono::seconds(0)) !=
                                        std::future_status::ready) {
            continue;
        }
        std::error_code error;
        try {
            error = state->pending_save->result.get();
        } catch (const std::system_error& exception) {
            error = exception.code();
        } catch (const std::bad_alloc&) {
            error = std::make_error_code(std::errc::not_enough_memory);
        } catch (...) {
            error = std::make_error_code(std::errc::io_error);
        }
        const Buffer& buffer = runtime_.buffers().get(state->buffer);
        const std::string display = buffer.resource_uri().value_or(buffer.name());
        if (error) {
            message_ = std::format("save failed: {}", error.message());
        } else {
            mark_saved(state->buffer, std::move(state->pending_save->content));
            message_ = buffer.modified() ? std::format("saved {} · newer edits remain", display)
                                         : std::format("saved {}", display);
        }
        state->pending_save.reset();
        changed = true;
    }
    return changed;
}

void EditorApplication::request_quit(bool force) {
    const std::size_t modified = static_cast<std::size_t>(
        std::ranges::count_if(buffers_, [this](const std::unique_ptr<BufferState>& state) {
            return runtime_.buffers().get(state->buffer).modified();
        }));
    if (modified == 0 || force || quit_armed_) {
        quit_ = true;
        return;
    }
    quit_armed_ = true;
    message_ = std::format("{} unsaved buffer{} · C-x C-s saves current · "
                           "C-x C-c again discards",
                           modified, modified == 1 ? "" : "s");
}

void EditorApplication::mark_saved(Text content) {
    mark_saved(buffer_id(), std::move(content));
}

EditorApplication::BufferState& EditorApplication::active_buffer() {
    return state_for(buffer_id());
}

const EditorApplication::BufferState& EditorApplication::active_buffer() const {
    return const_cast<EditorApplication*>(this)->active_buffer();
}

EditorApplication::BufferState& EditorApplication::state_for(BufferId buffer) {
    const auto found =
        std::ranges::find_if(buffers_, [buffer](const std::unique_ptr<BufferState>& state) {
            return state->buffer == buffer;
        });
    if (found == buffers_.end()) {
        throw std::out_of_range("buffer is not open in this application");
    }
    return **found;
}

const EditorApplication::BufferState& EditorApplication::state_for(BufferId buffer) const {
    return const_cast<EditorApplication*>(this)->state_for(buffer);
}

EditorApplication::ViewState& EditorApplication::active_view() {
    return view_state_for(view_id());
}

const EditorApplication::ViewState& EditorApplication::active_view() const {
    return const_cast<EditorApplication*>(this)->active_view();
}

EditorApplication::ViewState& EditorApplication::view_state_for(ViewId view) {
    const auto found = std::ranges::find_if(
        views_, [view](const std::unique_ptr<ViewState>& state) { return state->view == view; });
    if (found == views_.end()) {
        throw std::out_of_range("view has no editor session");
    }
    return **found;
}

const EditorApplication::ViewState& EditorApplication::view_state_for(ViewId view) const {
    return const_cast<EditorApplication*>(this)->view_state_for(view);
}

EditorApplication::ViewState* EditorApplication::find_view(WindowId window, BufferId buffer) {
    const auto found = std::ranges::find_if(views_, [&](const std::unique_ptr<ViewState>& state) {
        return state->window == window && state->buffer == buffer;
    });
    return found == views_.end() ? nullptr : found->get();
}

const EditorApplication::ViewState* EditorApplication::find_view(WindowId window,
                                                                 BufferId buffer) const {
    return const_cast<EditorApplication*>(this)->find_view(window, buffer);
}

EditSession& EditorApplication::session_for(ViewId view) {
    return *view_state_for(view).session;
}

const EditSession& EditorApplication::session_for(ViewId view) const {
    return const_cast<EditorApplication*>(this)->session_for(view);
}

BufferId EditorApplication::create_buffer(BufferSpec spec, CppIndentStyle style,
                                          std::string style_origin, TextOffset caret) {
    (void)caret;
    const CppModeRegistration cpp = ensure_cpp_mode(runtime_);
    const BufferId buffer = runtime_.buffers().create(std::move(spec));
    try {
        runtime_.buffers().get(buffer).modes().set_major(runtime_.modes(), cpp.mode);
        auto state = std::make_unique<BufferState>();
        state->buffer = buffer;
        state->style = std::make_shared<CppIndentStyle>(style);
        state->style_origin = std::move(style_origin);
        buffers_.push_back(std::move(state));
    } catch (...) {
        (void)runtime_.buffers().erase(buffer);
        throw;
    }
    return buffer;
}

ViewId EditorApplication::create_view(WindowId window, BufferId buffer, TextOffset caret) {
    BufferState& buffer_state = state_for(buffer);
    const ViewId view = runtime_.views().create(buffer, caret);
    try {
        auto state = std::make_unique<ViewState>();
        state->window = window;
        state->buffer = buffer;
        state->view = view;
        state->session = std::make_unique<EditSession>(runtime_, buffer, view, buffer_state.style);
        views_.push_back(std::move(state));
    } catch (...) {
        (void)runtime_.views().erase(view);
        throw;
    }
    return view;
}

bool EditorApplication::show_buffer(WindowId window, BufferId buffer) {
    if (runtime_.windows().try_get(window) == nullptr ||
        runtime_.buffers().try_get(buffer) == nullptr) {
        return false;
    }
    ViewState* view = find_view(window, buffer);
    if (view == nullptr) {
        const ViewId created = create_view(window, buffer);
        view = &view_state_for(created);
    }
    runtime_.windows().set_view(window, view->view);
    reveal_caret_ = true;
    quit_armed_ = false;
    if (window == active_window_) {
        sync_keymaps();
    }
    return true;
}

BufferId EditorApplication::create_scratch_buffer() {
    return create_buffer(BufferSpec{.name = "*scratch*",
                                    .initial_text = {},
                                    .kind = BufferKind::Scratch,
                                    .resource_uri = std::nullopt,
                                    .read_only = false},
                         CppIndentStyle{}, "llvm (fallback)");
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
    define("keyboard.quit", [this](const CommandInvocation&) {
        if (interaction_.cancel()) {
            message_ = "cancelled";
            return;
        }
        session().clear_selection();
        active_view().selection_history.clear();
        message_ = "cancelled";
    });
    const auto interaction_enabled = [this](const CommandContext&) {
        return interaction_.active();
    };
    runtime_.commands().define(
        "interaction.submit",
        [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            std::expected<InteractionSubmission, std::string> submission = interaction_.submit();
            if (!submission) {
                return std::unexpected(CommandError{std::move(submission.error())});
            }
            return CommandDispatch{.command = submission->accept_command,
                                   .invocation = std::move(submission->invocation)};
        },
        interaction_enabled);
    runtime_.commands().define(
        "interaction.erase-backward",
        [this](CommandContext& context, const CommandInvocation&) -> CommandResult {
            (void)interaction_.erase_backward(context);
            return CommandCompleted{};
        },
        interaction_enabled);
    runtime_.commands().define(
        "interaction.erase-forward",
        [this](CommandContext& context, const CommandInvocation&) -> CommandResult {
            (void)interaction_.erase_forward(context);
            return CommandCompleted{};
        },
        interaction_enabled);
    runtime_.commands().define(
        "interaction.backward-character",
        [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            (void)interaction_.move_backward();
            return CommandCompleted{};
        },
        interaction_enabled);
    runtime_.commands().define(
        "interaction.forward-character",
        [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            (void)interaction_.move_forward();
            return CommandCompleted{};
        },
        interaction_enabled);
    runtime_.commands().define(
        "interaction.line-start",
        [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            (void)interaction_.move_to_start();
            return CommandCompleted{};
        },
        interaction_enabled);
    runtime_.commands().define(
        "interaction.line-end",
        [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            (void)interaction_.move_to_end();
            return CommandCompleted{};
        },
        interaction_enabled);
    runtime_.commands().define(
        "interaction.next-candidate",
        [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            (void)interaction_.move_selection(1);
            return CommandCompleted{};
        },
        [this](const CommandContext&) {
            const InteractionState* state = interaction_.state();
            return state != nullptr && state->request.kind == InteractionKind::Picker;
        });
    runtime_.commands().define(
        "interaction.previous-candidate",
        [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            (void)interaction_.move_selection(-1);
            return CommandCompleted{};
        },
        [this](const CommandContext&) {
            const InteractionState* state = interaction_.state();
            return state != nullptr && state->request.kind == InteractionKind::Picker;
        });
    define("editor.redraw", [this](const CommandInvocation&) { reveal_caret_ = true; });
    define("editor.position", [this](const CommandInvocation&) {
        const DocumentSnapshot snapshot = session().snapshot();
        const Text& text = snapshot.content();
        const LinePosition position = text.position(session().caret());
        message_ = std::format(
            "line {}/{}, column {}, byte {}/{}", position.line + 1, text.line_count(),
            ui::display_column(text, session().caret(), session().style().tab_width) + 1,
            session().caret().value, text.size_bytes());
    });
    define("selection.toggle-mark", [this](const CommandInvocation&) {
        if (session().mark() && *session().mark() == session().caret()) {
            session().clear_selection();
            message_ = "mark cleared";
        } else {
            session().set_selection({.anchor = session().caret(), .head = session().caret()});
            active_view().selection_history.clear();
            message_ = "mark set";
        }
    });
    define("edit.kill-region", [this](const CommandInvocation&) {
        const std::optional<TextRange> selection = session().selection();
        if (!selection) {
            message_ = "no active region";
            return;
        }
        const std::optional<std::string> clipboard_error =
            store_kill(session().snapshot().substring(*selection));
        session().erase(*selection);
        active_view().selection_history.clear();
        after_edit();
        if (clipboard_error) {
            message_ = std::format("killed internally; clipboard: {}", *clipboard_error);
        }
    });
    define("edit.kill-line", [this](const CommandInvocation&) {
        const DocumentSnapshot snapshot = session().snapshot();
        const TextRange range =
            soft_kill_end(session().analysis().tree, snapshot.content(), session().caret());
        if (range.empty()) {
            return;
        }
        const std::optional<std::string> clipboard_error = store_kill(snapshot.substring(range));
        session().erase(range);
        active_view().selection_history.clear();
        after_edit();
        if (clipboard_error) {
            message_ = std::format("killed internally; clipboard: {}", *clipboard_error);
        }
    });
    define("edit.copy-region", [this](const CommandInvocation&) {
        const std::optional<TextRange> selection = session().selection();
        if (!selection) {
            message_ = "no active region";
            return;
        }
        const std::optional<std::string> clipboard_error =
            store_kill(session().snapshot().substring(*selection));
        session().clear_selection();
        message_ = clipboard_error
                       ? std::format("copied internally; clipboard: {}", *clipboard_error)
                       : "copied";
    });
    define("edit.yank", [this](const CommandInvocation&) {
        if (kill_slot_.empty()) {
            if (const std::optional<std::string> clipboard_error = import_clipboard()) {
                message_ = std::format("kill ring is empty; clipboard: {}", *clipboard_error);
                return;
            }
            if (kill_slot_.empty()) {
                message_ = "kill ring and clipboard are empty";
                return;
            }
        }
        session().insert_text(kill_slot_);
        after_edit();
    });

    auto define_structural_move = [this](std::string name, auto target) {
        runtime_.commands().define(
            std::move(name),
            [this, target = std::move(target)](CommandContext& context,
                                               const CommandInvocation&) -> CommandResult {
                EditSession& active = session_for(context.view_id());
                basic_commands_.reset_preferred_column(context.view_id());
                if (const std::optional<TextRange> range =
                        target(active.analysis().tree, active.caret())) {
                    active.set_caret(range->start);
                    reveal_caret_ = true;
                }
                return CommandCompleted{};
            });
    };
    define_structural_move("cursor.forward-expression",
                           [](const SyntaxTree& tree, TextOffset caret) {
                               std::optional<TextRange> range = sexp_forward(tree, caret);
                               if (range) {
                                   range->start = range->end;
                               }
                               return range;
                           });
    define_structural_move(
        "cursor.backward-expression",
        [](const SyntaxTree& tree, TextOffset caret) { return sexp_backward(tree, caret); });
    define_structural_move("cursor.up-list", [](const SyntaxTree& tree, TextOffset caret) {
        return enclosing_list(tree, caret);
    });
    define("selection.expand", [this](const CommandInvocation&) {
        const TextRange current =
            session().selection().value_or(TextRange{session().caret(), session().caret()});
        if (const std::optional<TextRange> next =
                expand_selection(session().analysis().tree, current)) {
            if (session().selection()) {
                active_view().selection_history.push_back(current);
            }
            session().set_selection({.anchor = next->start, .head = next->end});
        }
    });
    define("selection.contract", [this](const CommandInvocation&) {
        std::vector<TextRange>& history = active_view().selection_history;
        if (history.empty()) {
            session().clear_selection();
            return;
        }
        const TextRange previous = history.back();
        history.pop_back();
        session().set_selection({.anchor = previous.start, .head = previous.end});
    });

    command_palette_accept_ = runtime_.commands().define(
        "command.palette.accept",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept_command_palette(context, invocation);
        });
    runtime_.commands().define(
        "command.palette", [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin_command_palette(context, invocation);
        });

    open_file_accept_ = runtime_.commands().define(
        "file.open.accept", [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept_open_file(context, invocation);
        });
    runtime_.commands().define(
        "file.open", [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin_open_file(context, invocation);
        });
    save_as_accept_ = runtime_.commands().define(
        "file.save-as.accept",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept_save_as(context, invocation);
        });
    runtime_.commands().define(
        "file.save-as", [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin_save_as(context, invocation);
        });

    switch_buffer_accept_ = runtime_.commands().define(
        "buffer.switch.accept",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept_switch_buffer(context, invocation);
        });
    runtime_.commands().define(
        "buffer.switch", [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin_switch_buffer(context, invocation);
        });
    define("buffer.next", [this](const CommandInvocation&) { switch_relative(1); });
    define("buffer.previous", [this](const CommandInvocation&) { switch_relative(-1); });
    runtime_.commands().define(
        "buffer.kill", [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            std::expected<void, std::string> result = kill_buffer(buffer_id());
            return result ? CommandResult{CommandCompleted{}}
                          : CommandResult{std::unexpected(CommandError{result.error()})};
        });
    runtime_.commands().define(
        "buffer.force-kill", [this](CommandContext&, const CommandInvocation&) -> CommandResult {
            std::expected<void, std::string> result = kill_buffer(buffer_id(), true);
            return result ? CommandResult{CommandCompleted{}}
                          : CommandResult{std::unexpected(CommandError{result.error()})};
        });

    goto_line_accept_ = runtime_.commands().define(
        "cursor.goto-line.accept",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept_goto_line(context, invocation);
        });
    runtime_.commands().define(
        "cursor.goto-line", [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin_goto_line(context, invocation);
        });

    help_keys_accept_ = runtime_.commands().define(
        "help.keys.accept",
        [this](CommandContext&, const CommandInvocation& invocation) -> CommandResult {
            const std::string* binding = submitted_string(invocation);
            if (binding != nullptr) {
                message_ = *binding;
            }
            return CommandCompleted{};
        });
    runtime_.commands().define("help.keys",
                               [this](CommandContext&, const CommandInvocation&) -> CommandResult {
                                   return InteractionRequest{.kind = InteractionKind::Picker,
                                                             .prompt = "Key bindings: ",
                                                             .initial_input = {},
                                                             .history = "key-bindings",
                                                             .provider = "key-bindings",
                                                             .allow_custom_input = false,
                                                             .accept_command = help_keys_accept_,
                                                             .arguments = {}};
                               });
}

void EditorApplication::register_interaction_providers() {
    runtime_.interaction_providers().define("commands", [this](CommandContext& context,
                                                               std::string_view) {
        std::vector<InteractionCandidate> candidates;
        for (const CommandId command : runtime_.commands().all()) {
            const CommandRegistry::Definition& definition = runtime_.commands().definition(command);
            if (internal_command(definition.name) ||
                !runtime_.commands().enabled(command, context)) {
                continue;
            }
            candidates.push_back({.value = definition.name,
                                  .label = definition.name,
                                  .detail = "command",
                                  .filter_text = definition.name});
        }
        return candidates;
    });
    runtime_.interaction_providers().define("buffers", [this](CommandContext&, std::string_view) {
        std::vector<InteractionCandidate> candidates;
        for (const OpenBufferSnapshot& buffer : open_buffers()) {
            std::string detail = buffer.resource.value_or(std::string());
            if (buffer.modified) {
                if (detail.empty()) {
                    detail = "modified";
                } else {
                    detail.append(" · modified");
                }
            }
            candidates.push_back({.value = buffer.name,
                                  .label = buffer.name,
                                  .detail = detail,
                                  .filter_text = buffer.resource
                                                     ? buffer.name + " " + *buffer.resource
                                                     : buffer.name});
        }
        return candidates;
    });
    runtime_.interaction_providers().define(
        "files", [this](CommandContext&, std::string_view query) {
            fs::path typed(query);
            fs::path directory;
            if (query.empty()) {
                const Buffer& buffer = session().buffer();
                directory = buffer.resource_uri() ? fs::path(*buffer.resource_uri()).parent_path()
                                                  : fs::current_path();
            } else if (query.ends_with(fs::path::preferred_separator)) {
                directory = typed;
            } else {
                directory = typed.has_parent_path() ? typed.parent_path() : fs::current_path();
            }
            std::error_code error;
            directory = fs::absolute(directory, error).lexically_normal();
            if (error || !fs::is_directory(directory, error)) {
                return std::vector<InteractionCandidate>{};
            }

            std::vector<InteractionCandidate> candidates;
            if (directory.has_parent_path()) {
                const std::string parent = directory_input(directory.parent_path());
                candidates.push_back({.value = parent,
                                      .label = "../",
                                      .detail = directory.parent_path().string(),
                                      .filter_text = parent});
            }
            for (fs::directory_iterator iterator(directory, error), end;
                 !error && iterator != end && candidates.size() < 1000; iterator.increment(error)) {
                const fs::directory_entry& entry = *iterator;
                const bool is_directory = entry.is_directory(error);
                std::string value = entry.path().lexically_normal().string();
                std::string label = entry.path().filename().string();
                if (is_directory) {
                    value = directory_input(value);
                    label.push_back(fs::path::preferred_separator);
                }
                candidates.push_back({.value = value,
                                      .label = std::move(label),
                                      .detail = directory.string(),
                                      .filter_text = value});
            }
            return candidates;
        });
    runtime_.interaction_providers().define("key-bindings", [this](CommandContext&,
                                                                   std::string_view) {
        std::vector<InteractionCandidate> candidates;
        for (const KeymapLayer& layer : command_loop_.keymap_layers()) {
            const KeymapId keymap = layer.keymap;
            for (const KeymapBinding& binding : runtime_.keymaps().bindings(keymap)) {
                const std::string keys = format_key_sequence(binding.sequence);
                const std::string& command = runtime_.commands().definition(binding.command).name;
                std::string value = keys;
                value.append("  ").append(command);
                std::string filter_text = keys;
                filter_text.push_back(' ');
                filter_text.append(command);
                candidates.push_back({.value = std::move(value),
                                      .label = keys,
                                      .detail = command,
                                      .filter_text = std::move(filter_text)});
            }
        }
        return candidates;
    });
}

void EditorApplication::register_keymaps() {
    keymap_ = runtime_.keymaps().define("editor.default");
    application_keymap_ = runtime_.keymaps().define("application.global");
    system_keymap_ = runtime_.keymaps().define("editor.system");
    interaction_text_keymap_ = runtime_.keymaps().define("interaction.text");
    interaction_picker_keymap_ = runtime_.keymaps().define("interaction.picker");
    runtime_.keymaps().set_parent(interaction_picker_keymap_, interaction_text_keymap_);
    refresh_default_keymap();
    (void)bind_default_application_keys(runtime_, application_keymap_);

    const auto command = [this](std::string_view name) {
        const std::optional<CommandId> id = runtime_.commands().find(name);
        if (!id) {
            throw std::logic_error(std::format("missing built-in command '{}'", name));
        }
        return *id;
    };
    runtime_.keymaps().bind(system_keymap_, "C-g", command("keyboard.quit"));
    for (const auto& [keys, name] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"RET", "interaction.submit"},
             {"Backspace", "interaction.erase-backward"},
             {"C-d", "interaction.erase-forward"},
             {"Delete", "interaction.erase-forward"},
             {"C-b", "interaction.backward-character"},
             {"Left", "interaction.backward-character"},
             {"C-f", "interaction.forward-character"},
             {"Right", "interaction.forward-character"},
             {"C-a", "interaction.line-start"},
             {"Home", "interaction.line-start"},
             {"C-e", "interaction.line-end"},
             {"End", "interaction.line-end"},
             {"ESC", "keyboard.quit"},
         }) {
        runtime_.keymaps().bind(interaction_text_keymap_, keys, command(name));
    }
    for (std::string_view keys : {"C-n", "Down", "TAB"}) {
        runtime_.keymaps().bind(interaction_picker_keymap_, keys,
                                command("interaction.next-candidate"));
    }
    for (std::string_view keys : {"C-p", "Up"}) {
        runtime_.keymaps().bind(interaction_picker_keymap_, keys,
                                command("interaction.previous-candidate"));
    }
    command_loop_.set_override_keymaps({system_keymap_});
}

std::vector<KeymapLayer> EditorApplication::window_keymap_layers() const {
    std::vector<KeymapLayer> layers;
    const auto append = [&](std::span<const KeymapId> maps, std::string_view scope) {
        for (const KeymapId map : maps) {
            if (std::ranges::any_of(
                    layers, [map](const KeymapLayer& layer) { return layer.keymap == map; })) {
                continue;
            }
            layers.push_back({.keymap = map, .scope = std::string(scope)});
        }
    };

    const Window& window = runtime_.windows().get(active_window_);
    const View& view = runtime_.views().get(window.view_id());
    const Buffer& buffer = runtime_.buffers().get(view.buffer_id());
    append(window.keymaps(), "window");
    append(view.keymaps(), "view");
    append(buffer.keymaps(), "buffer");
    for (auto mode = buffer.modes().minors().rbegin(); mode != buffer.modes().minors().rend();
         ++mode) {
        const ModeRegistry::Definition& definition = runtime_.modes().definition(*mode);
        append(definition.keymaps, std::format("minor-mode:{}", definition.name));
    }
    if (buffer.modes().major()) {
        const ModeRegistry::Definition& definition =
            runtime_.modes().definition(*buffer.modes().major());
        append(definition.keymaps, std::format("major-mode:{}", definition.name));
    }
    append(std::span(&keymap_, 1), "editor");
    append(std::span(&application_keymap_, 1), "global");
    return layers;
}

void EditorApplication::sync_keymaps() {
    std::vector<KeymapLayer> layers;
    if (const InteractionState* interaction = interaction_.state()) {
        layers.push_back({.keymap = interaction->request.kind == InteractionKind::Picker
                                        ? interaction_picker_keymap_
                                        : interaction_text_keymap_,
                          .scope = "interaction"});
        layers.push_back({.keymap = application_keymap_, .scope = "global"});
    } else {
        layers = window_keymap_layers();
    }
    const std::span<const KeymapLayer> active = command_loop_.keymap_layers();
    const bool changed =
        layers.size() != active.size() ||
        !std::ranges::equal(layers, active, [](const KeymapLayer& left, const KeymapLayer& right) {
            return left.keymap == right.keymap && left.scope == right.scope;
        });
    if (!changed) {
        return;
    }
    command_loop_.set_keymap_layers(std::move(layers));
}

bool EditorApplication::handle_loop_result(CommandLoopResult result) {
    if (result.command) {
        last_command_ = runtime_.commands().definition(*result.command).name;
    }
    if (result.interaction) {
        CommandContext context = command_context();
        std::expected<void, std::string> started =
            interaction_.start(std::move(*result.interaction), context);
        if (!started) {
            message_ = started.error();
        } else {
            message_.clear();
        }
    } else if (result.status == CommandLoopStatus::Prefix ||
               result.status == CommandLoopStatus::Error ||
               result.status == CommandLoopStatus::Disabled ||
               result.status == CommandLoopStatus::Cancelled ||
               (result.status == CommandLoopStatus::NotHandled && result.consumed)) {
        message_ = result.message;
    }
    return result.consumed;
}

CommandContext EditorApplication::command_context() {
    return CommandContext(runtime_, active_window_, buffer_id(), view_id());
}

void EditorApplication::after_edit() {
    session().clear_selection();
    active_view().selection_history.clear();
    quit_armed_ = false;
    message_.clear();
    reveal_caret_ = true;
}

std::optional<std::string> EditorApplication::store_kill(std::string text) {
    kill_slot_ = std::move(text);
    if (!platform_services_.write_clipboard) {
        return std::nullopt;
    }
    std::expected<void, std::string> result = platform_services_.write_clipboard(kill_slot_);
    if (!result) {
        return std::move(result.error());
    }
    return std::nullopt;
}

std::optional<std::string> EditorApplication::import_clipboard() {
    if (!platform_services_.read_clipboard) {
        return std::nullopt;
    }
    std::expected<std::string, std::string> result = platform_services_.read_clipboard();
    if (!result) {
        return std::move(result.error());
    }
    kill_slot_ = std::move(*result);
    return std::nullopt;
}

void EditorApplication::save() {
    BufferState& state = active_buffer();
    if (state.pending_save) {
        message_ = "save already in progress";
        return;
    }
    const std::optional<std::string>& resource =
        runtime_.buffers().get(state.buffer).resource_uri();
    if (!resource) {
        message_ = "buffer has no file path";
        return;
    }
    const DocumentSnapshot snapshot = runtime_.buffers().get(state.buffer).snapshot();
    Text content = snapshot.content();
    std::string target_path = *resource;
    try {
        state.pending_save.emplace(PendingSave{
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
        message_ = std::format("saving {}…", *resource);
    } catch (const std::system_error& exception) {
        message_ = std::format("save failed: {}", exception.code().message());
    } catch (const std::bad_alloc&) {
        message_ = "save failed: not enough memory";
    }
}

void EditorApplication::mark_saved(BufferId buffer, Text content) {
    BufferState& state = state_for(buffer);
    runtime_.buffers().get(buffer).mark_saved(std::move(content));
    ++state.save_generation;
    quit_armed_ = false;
}

void EditorApplication::switch_relative(int delta) {
    if (buffers_.size() < 2 || delta == 0) {
        return;
    }
    const auto active =
        std::ranges::find_if(buffers_, [this](const std::unique_ptr<BufferState>& state) {
            return state->buffer == buffer_id();
        });
    if (active == buffers_.end()) {
        throw std::logic_error("active window displays an untracked buffer");
    }
    const std::int64_t size = static_cast<std::int64_t>(buffers_.size());
    std::int64_t next =
        (static_cast<std::int64_t>(active - buffers_.begin()) + static_cast<std::int64_t>(delta)) %
        size;
    if (next < 0) {
        next += size;
    }
    (void)switch_buffer(buffers_[static_cast<std::size_t>(next)]->buffer);
}

CommandResult EditorApplication::begin_command_palette(CommandContext&,
                                                       const CommandInvocation&) const {
    return InteractionRequest{.kind = InteractionKind::Picker,
                              .prompt = "Command: ",
                              .initial_input = {},
                              .history = "commands",
                              .provider = "commands",
                              .allow_custom_input = false,
                              .accept_command = command_palette_accept_,
                              .arguments = {}};
}

CommandResult EditorApplication::accept_command_palette(CommandContext& context,
                                                        const CommandInvocation& invocation) {
    const std::string* name = submitted_string(invocation);
    if (name == nullptr) {
        return std::unexpected(CommandError{"command palette requires a command name"});
    }
    const std::optional<CommandId> command = runtime_.commands().find(*name);
    if (!command || *command == command_palette_accept_) {
        return std::unexpected(CommandError{std::format("unknown command '{}'", *name)});
    }
    return runtime_.commands().invoke(*command, context);
}

CommandResult EditorApplication::begin_open_file(CommandContext&, const CommandInvocation&) const {
    const Buffer& buffer = session().buffer();
    const fs::path directory =
        buffer.resource_uri() ? fs::path(*buffer.resource_uri()).parent_path() : fs::current_path();
    return InteractionRequest{.kind = InteractionKind::Picker,
                              .prompt = "Open file: ",
                              .initial_input = directory_input(directory),
                              .history = "files",
                              .provider = "files",
                              .allow_custom_input = true,
                              .accept_command = open_file_accept_,
                              .arguments = {}};
}

CommandResult EditorApplication::accept_open_file(CommandContext&,
                                                  const CommandInvocation& invocation) {
    const std::string* input = submitted_string(invocation);
    if (input == nullptr) {
        return std::unexpected(CommandError{"open file requires a path"});
    }
    std::error_code error;
    if (fs::is_directory(*input, error)) {
        return InteractionRequest{.kind = InteractionKind::Picker,
                                  .prompt = "Open file: ",
                                  .initial_input = directory_input(*input),
                                  .history = "files",
                                  .provider = "files",
                                  .allow_custom_input = true,
                                  .accept_command = open_file_accept_,
                                  .arguments = {}};
    }
    std::expected<BufferId, std::string> opened = open_file(*input);
    return opened ? CommandResult{CommandCompleted{}}
                  : CommandResult{std::unexpected(CommandError{opened.error()})};
}

CommandResult EditorApplication::begin_save_as(CommandContext&, const CommandInvocation&) const {
    const Buffer& buffer = session().buffer();
    return InteractionRequest{.kind = InteractionKind::Text,
                              .prompt = "Write file: ",
                              .initial_input = buffer.resource_uri().value_or(std::string()),
                              .history = "files",
                              .provider = {},
                              .allow_custom_input = true,
                              .accept_command = save_as_accept_,
                              .arguments = {}};
}

CommandResult EditorApplication::accept_save_as(CommandContext& context,
                                                const CommandInvocation& invocation) {
    const std::string* input = submitted_string(invocation);
    if (input == nullptr) {
        return std::unexpected(CommandError{"write file requires a path"});
    }
    std::expected<std::string, std::string> path = normalized_path(*input);
    if (!path) {
        return std::unexpected(CommandError{path.error()});
    }
    try {
        runtime_.buffers().set_resource(context.buffer_id(), *path, BufferKind::File);
        runtime_.buffers().rename(context.buffer_id(), fs::path(*path).filename().string());
    } catch (const std::exception& exception) {
        return std::unexpected(CommandError{exception.what()});
    }
    save();
    return CommandCompleted{};
}

CommandResult EditorApplication::begin_switch_buffer(CommandContext&,
                                                     const CommandInvocation&) const {
    return InteractionRequest{.kind = InteractionKind::Picker,
                              .prompt = "Switch buffer: ",
                              .initial_input = {},
                              .history = "buffers",
                              .provider = "buffers",
                              .allow_custom_input = false,
                              .accept_command = switch_buffer_accept_,
                              .arguments = {}};
}

CommandResult EditorApplication::accept_switch_buffer(CommandContext&,
                                                      const CommandInvocation& invocation) {
    const std::string* name = submitted_string(invocation);
    if (name == nullptr) {
        return std::unexpected(CommandError{"switch buffer requires a buffer name"});
    }
    const std::optional<BufferId> buffer = runtime_.buffers().find_by_name(*name);
    if (!buffer || !switch_buffer(*buffer)) {
        return std::unexpected(CommandError{std::format("unknown buffer '{}'", *name)});
    }
    return CommandCompleted{};
}

CommandResult EditorApplication::begin_goto_line(CommandContext&, const CommandInvocation&) const {
    return InteractionRequest{.kind = InteractionKind::Text,
                              .prompt = "Go to line: ",
                              .initial_input = {},
                              .history = "line-numbers",
                              .provider = {},
                              .allow_custom_input = true,
                              .accept_command = goto_line_accept_,
                              .arguments = {}};
}

CommandResult EditorApplication::accept_goto_line(CommandContext& context,
                                                  const CommandInvocation& invocation) {
    const std::string* input = submitted_string(invocation);
    if (input == nullptr || input->empty()) {
        return std::unexpected(CommandError{"line number is empty"});
    }
    std::string_view line_text = *input;
    std::string_view column_text;
    if (const std::size_t separator = line_text.find_first_of(":,");
        separator != std::string_view::npos) {
        column_text = line_text.substr(separator + 1);
        line_text = line_text.substr(0, separator);
    }
    std::uint32_t line = 0;
    const auto [line_end, line_error] =
        std::from_chars(line_text.data(), line_text.data() + line_text.size(), line);
    if (line_error != std::errc() || line_end != line_text.data() + line_text.size() || line == 0) {
        return std::unexpected(CommandError{"invalid line number"});
    }
    std::uint32_t column = 1;
    if (!column_text.empty()) {
        const auto [column_end, column_error] =
            std::from_chars(column_text.data(), column_text.data() + column_text.size(), column);
        if (column_error != std::errc() || column_end != column_text.data() + column_text.size() ||
            column == 0) {
            return std::unexpected(CommandError{"invalid column number"});
        }
    }

    EditSession& active = session_for(context.view_id());
    const DocumentSnapshot snapshot = active.snapshot();
    const std::uint32_t target_line = std::min(line - 1, snapshot.content().line_count() - 1);
    active.set_caret(ui::offset_at_display_column(
        snapshot.content(),
        {.line = target_line,
         .column = static_cast<int>(std::min<std::uint32_t>(
             column - 1, static_cast<std::uint32_t>(std::numeric_limits<int>::max())))},
        active.style().tab_width));
    basic_commands_.reset_preferred_column(context.view_id());
    reveal_caret_ = true;
    return CommandCompleted{};
}

} // namespace cind
