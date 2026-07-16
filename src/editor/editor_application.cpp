#include "editor/editor_application.hpp"

#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/default_keymap.hpp"
#include "project/project_files.hpp"
#include "syntax/structure.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <initializer_list>
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

struct LoadedFileData {
    std::string resource;
    std::string contents;
    CppIndentStyle style;
    std::string style_origin;
    std::optional<ProjectDiscovery> project;
};

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
      command_loop_(runtime_), platform_services_(std::move(spec.platform_services)),
      async_runtime_(std::move(platform_services_.wake_event_loop)) {
    interaction_.attach_async_runtime(async_runtime_);
    project_service_ =
        std::make_unique<ProjectService>(runtime_, async_runtime_, [this](ProjectId project) {
            if (InteractionState* active = interaction_.state();
                active != nullptr && active->request.provider == "project-files" &&
                session().buffer().project_id() == project) {
                CommandContext context = command_context();
                interaction_.refresh_candidates(context);
            }
        });
    register_commands();
    register_interaction_providers();

    const bool deferred_initial_load = !spec.initial_text && !spec.path.empty();
    BufferSpec initial_buffer{
        .name = {},
        .initial_text = spec.initial_text ? std::move(*spec.initial_text) : std::string(),
        .kind = deferred_initial_load || spec.path.empty() ? BufferKind::Scratch : BufferKind::File,
        .resource_uri = std::nullopt,
        .read_only = false};
    if (!spec.path.empty() && !deferred_initial_load) {
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
    window_layout_ = WindowLayout(active_window_);
    if (spec.initial_line > 0 && !deferred_initial_load) {
        const DocumentSnapshot snapshot = session().snapshot();
        const std::uint32_t line =
            std::min(spec.initial_line - 1, snapshot.content().line_count() - 1);
        session().set_caret(snapshot.content().line_start(line));
    }

    register_keymaps();
    sync_keymaps();
    if (deferred_initial_load) {
        startup_placeholder_ = initial;
        if (std::expected<void, std::string> opened =
                open_file(spec.path, active_window_, spec.initial_line);
            !opened) {
            message_ = std::format("open failed: {}", opened.error());
        }
    }
}

BufferId EditorApplication::buffer_id() const {
    return runtime_.views().get(view_id()).buffer_id();
}

BufferId EditorApplication::buffer_id(WindowId window) const {
    return runtime_.views().get(view_id(window)).buffer_id();
}

ViewId EditorApplication::view_id() const {
    return runtime_.windows().get(active_window_).view_id();
}

ViewId EditorApplication::view_id(WindowId window) const {
    return runtime_.windows().get(window).view_id();
}

EditSession& EditorApplication::session() {
    return *active_view().session;
}

const EditSession& EditorApplication::session() const {
    return *active_view().session;
}

EditSession& EditorApplication::session(WindowId window) {
    return session_for(view_id(window));
}

const EditSession& EditorApplication::session(WindowId window) const {
    return session_for(view_id(window));
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

std::expected<void, std::string> EditorApplication::open_file(std::string_view input) {
    return open_file(input, active_window_, 0);
}

std::expected<void, std::string> EditorApplication::open_file(std::string_view input,
                                                              WindowId target_window,
                                                              std::uint32_t initial_line) {
    std::expected<std::string, std::string> normalized = normalized_path(input);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    if (const std::optional<BufferId> existing = runtime_.buffers().find_by_resource(*normalized)) {
        (void)show_buffer(target_window, *existing);
        return {};
    }
    if (auto pending = std::ranges::find_if(
            pending_opens_, [&](const PendingOpen& open) { return open.resource == *normalized; });
        pending != pending_opens_.end()) {
        pending->target_window = target_window;
        pending->initial_line = initial_line;
        message_ = std::format("opening {}…", *normalized);
        return {};
    }

    const std::string resource = *normalized;
    try {
        pending_opens_.push_back({.resource = resource,
                                  .target_window = target_window,
                                  .initial_line = initial_line,
                                  .task = {}});
        PendingOpen& pending = pending_opens_.back();
        auto requested_resource = std::make_shared<const std::string>(resource);
        pending.task = async_runtime_.submit({
            .work = [this,
                     requested_resource](const std::stop_token& cancellation) -> AsyncCompletion {
                std::expected<FileReadResult, std::error_code> file =
                    read_file_contents(*requested_resource, cancellation);
                if (!file) {
                    if (file.error() == std::make_error_code(std::errc::operation_canceled)) {
                        throw AsyncTaskCancelled();
                    }
                    throw std::system_error(file.error(),
                                            std::format("cannot open {}", *requested_resource));
                }
                auto loaded_data = std::make_shared<LoadedFileData>(LoadedFileData{
                    .resource = *requested_resource,
                    .contents = std::move(file->contents),
                    .style = {},
                    .style_origin = "llvm (fallback)",
                    .project = std::nullopt,
                });
                if (std::optional<LoadedStyle> loaded_style =
                        load_clang_format_style(fs::path(*requested_resource).parent_path())) {
                    loaded_data->style = loaded_style->style;
                    loaded_data->style_origin = loaded_style->config_path.filename().string();
                }
                std::expected<std::optional<ProjectDiscovery>, std::error_code> project =
                    discover_project(*requested_resource, cancellation);
                if (!project &&
                    project.error() == std::make_error_code(std::errc::operation_canceled)) {
                    throw AsyncTaskCancelled();
                }
                if (project) {
                    loaded_data->project = std::move(*project);
                }
                return [this, loaded_data] {
                    finish_open(std::move(loaded_data->resource), std::move(loaded_data->contents),
                                loaded_data->style, std::move(loaded_data->style_origin),
                                loaded_data->project);
                };
            },
            .cancelled = [this, requested_resource] { cancel_open(*requested_resource); },
            .failed =
                [this, requested_resource](const std::exception_ptr& failure) {
                    fail_open(*requested_resource, failure);
                },
        });
        message_ = std::format("opening {}…", resource);
        return {};
    } catch (const std::exception& exception) {
        std::erase_if(pending_opens_,
                      [&](const PendingOpen& open) { return open.resource == resource; });
        return std::unexpected(exception.what());
    }
}

void EditorApplication::finish_open(std::string resource, std::string contents,
                                    CppIndentStyle style, std::string style_origin,
                                    const std::optional<ProjectDiscovery>& project) {
    const auto pending = std::ranges::find_if(
        pending_opens_, [&](const PendingOpen& open) { return open.resource == resource; });
    if (pending == pending_opens_.end()) {
        return;
    }
    const WindowId requested_window = pending->target_window;
    const std::uint32_t initial_line = pending->initial_line;
    pending_opens_.erase(pending);

    try {
        BufferId buffer;
        if (const std::optional<BufferId> existing =
                runtime_.buffers().find_by_resource(resource)) {
            buffer = *existing;
        } else {
            buffer = create_buffer(BufferSpec{.name = {},
                                              .initial_text = std::move(contents),
                                              .kind = BufferKind::File,
                                              .resource_uri = resource,
                                              .read_only = false},
                                   style, std::move(style_origin));
        }
        project_service_->attach_buffer(buffer, project);
        const WindowId target = runtime_.windows().try_get(requested_window) != nullptr
                                    ? requested_window
                                    : active_window_;
        if (runtime_.windows().try_get(target) != nullptr) {
            (void)show_buffer(target, buffer);
            if (initial_line > 0) {
                EditSession& opened = session(target);
                const DocumentSnapshot snapshot = opened.snapshot();
                const std::uint32_t line =
                    std::min(initial_line - 1, snapshot.content().line_count() - 1);
                opened.set_caret(snapshot.content().line_start(line));
            }
        }
        if (startup_placeholder_ && *startup_placeholder_ != buffer) {
            const BufferId placeholder = *startup_placeholder_;
            startup_placeholder_.reset();
            if (Buffer* startup = runtime_.buffers().try_get(placeholder);
                startup != nullptr && !startup->modified()) {
                std::expected<void, std::string> removed = kill_buffer(placeholder, true);
                if (!removed) {
                    startup_placeholder_ = placeholder;
                }
            }
        }
        message_ = std::format("opened {}", resource);
    } catch (const std::exception& exception) {
        message_ = std::format("open failed: {}", exception.what());
    }
}

std::expected<void, std::string> EditorApplication::start_project_search(ProjectId project,
                                                                         std::string query,
                                                                         WindowId target_window) {
    const Project* definition = runtime_.projects().try_get(project);
    if (definition == nullptr || definition->roots().empty()) {
        return std::unexpected("project has no root");
    }
    if (project_search_.process.valid()) {
        (void)async_runtime_.terminate(project_search_.process);
    }
    const std::uint64_t generation = ++project_search_.generation;
    const std::string root = definition->roots().front();
    try {
        project_search_.process = async_runtime_.spawn({
            .file = "rg",
            .arguments = {"--line-number", "--column", "--no-heading", "--color", "never",
                          "--smart-case", "--", query, "."},
            .working_directory = root,
            .completed =
                [this, project, target_window, generation,
                 query = std::move(query)](AsyncProcessResult result) mutable {
                    finish_project_search(project, target_window, generation, std::move(query),
                                          std::move(result));
                },
            .cancelled = [this, generation] { cancel_project_search(generation); },
            .failed =
                [this, generation](const std::exception_ptr& failure) {
                    fail_project_search(generation, failure);
                },
        });
        message_ = std::format("searching project {}…", definition->name());
        return {};
    } catch (const std::exception& exception) {
        project_search_.process = {};
        return std::unexpected(exception.what());
    }
}

void EditorApplication::finish_project_search(ProjectId project, WindowId target_window,
                                              std::uint64_t generation, std::string query,
                                              AsyncProcessResult result) {
    if (project_search_.generation != generation || project_search_.process != result.process) {
        return;
    }
    project_search_.process = {};
    if (result.exit_status > 1 || result.term_signal != 0) {
        std::string detail = std::move(result.standard_error);
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
            detail.pop_back();
        }
        message_ = detail.empty()
                       ? std::format("project search failed with status {}", result.exit_status)
                       : std::format("project search failed: {}", detail);
        return;
    }
    try {
        if (result.standard_output.empty()) {
            result.standard_output = std::format("No matches for: {}\n", query);
        }
        const BufferId buffer =
            create_buffer(BufferSpec{.name = std::format("*project grep: {}*", query),
                                     .initial_text = std::move(result.standard_output),
                                     .kind = BufferKind::Process,
                                     .resource_uri = std::nullopt,
                                     .read_only = true},
                          CppIndentStyle{}, "process output");
        runtime_.projects().assign(buffer, project);
        const WindowId target =
            runtime_.windows().try_get(target_window) != nullptr ? target_window : active_window_;
        (void)show_buffer(target, buffer);
        message_ = std::format("project search finished: {}", query);
    } catch (const std::exception& exception) {
        message_ = std::format("project search failed: {}", exception.what());
    }
}

void EditorApplication::fail_project_search(std::uint64_t generation,
                                            const std::exception_ptr& failure) {
    if (project_search_.generation != generation) {
        return;
    }
    project_search_.process = {};
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
        message_ = "project search failed";
    } catch (const std::exception& exception) {
        message_ = std::format("project search failed: {}", exception.what());
    } catch (...) {
        message_ = "project search failed";
    }
}

void EditorApplication::cancel_project_search(std::uint64_t generation) {
    if (project_search_.generation != generation) {
        return;
    }
    project_search_.process = {};
    message_ = "project search cancelled";
}

void EditorApplication::fail_open(std::string_view resource, const std::exception_ptr& failure) {
    std::erase_if(pending_opens_,
                  [&](const PendingOpen& open) { return open.resource == resource; });
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
        message_ = "open failed";
    } catch (const std::exception& exception) {
        message_ = std::format("open failed: {}", exception.what());
    } catch (...) {
        message_ = "open failed";
    }
}

void EditorApplication::cancel_open(std::string_view resource) {
    std::erase_if(pending_opens_,
                  [&](const PendingOpen& open) { return open.resource == resource; });
    message_ = std::format("open cancelled: {}", resource);
}

bool EditorApplication::switch_buffer(BufferId buffer) {
    return show_buffer(active_window_, buffer);
}

bool EditorApplication::focus_window(WindowId window) {
    if (!window_layout_.contains(window) || runtime_.windows().try_get(window) == nullptr) {
        return false;
    }
    active_window_ = window;
    reveal_caret_ = true;
    quit_armed_ = false;
    sync_keymaps();
    return true;
}

bool EditorApplication::split_window(WindowSplitAxis axis) {
    EditSession& source = session();
    const ViewportState source_viewport = source.view().viewport();
    const std::optional<SelectionEndpoints> source_selection =
        source.selection().transform([](TextRange range) {
            return SelectionEndpoints{.anchor = range.start, .head = range.end};
        });
    const ViewId view = create_view({}, buffer_id(), source.caret());
    runtime_.views().get(view).viewport() = source_viewport;
    if (source_selection) {
        runtime_.views().set_selection(view, *source_selection);
    }
    const WindowId window = runtime_.windows().create(view);
    view_state_for(view).window = window;
    if (!window_layout_.split({.target = active_window_, .new_window = window, .axis = axis})) {
        destroy_window(window);
        return false;
    }
    message_ = axis == WindowSplitAxis::Rows ? "window split below" : "window split right";
    reveal_caret_ = true;
    return true;
}

bool EditorApplication::delete_window() {
    const std::optional<WindowId> replacement = window_layout_.next(active_window_);
    if (!replacement || *replacement == active_window_ || !window_layout_.erase(active_window_)) {
        message_ = "cannot delete the only window";
        return false;
    }
    const WindowId removed = active_window_;
    active_window_ = *replacement;
    destroy_window(removed);
    message_ = "window deleted";
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

void EditorApplication::delete_other_windows() {
    if (window_layout_.leaves().size() <= 1) {
        message_ = "only window";
        return;
    }
    const std::vector<WindowId> windows(window_layout_.leaves().begin(),
                                        window_layout_.leaves().end());
    (void)window_layout_.retain(active_window_);
    for (const WindowId window : windows) {
        if (window != active_window_) {
            destroy_window(window);
        }
    }
    message_ = "other windows deleted";
    reveal_caret_ = true;
    sync_keymaps();
}

bool EditorApplication::select_other_window(int delta) {
    const std::optional<WindowId> target = window_layout_.next(active_window_, delta);
    if (!target || *target == active_window_) {
        message_ = "only window";
        return false;
    }
    return focus_window(*target);
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
    result.reserve(window_layout_.leaves().size());
    for (const WindowId window : window_layout_.leaves()) {
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

const std::string& EditorApplication::path(WindowId window) const {
    const Buffer& buffer = session(window).buffer();
    return buffer.resource_uri() ? *buffer.resource_uri() : buffer.name();
}

const std::string& EditorApplication::style_origin() const {
    return active_buffer().style_origin;
}

const std::string& EditorApplication::style_origin(WindowId window) const {
    return state_for(buffer_id(window)).style_origin;
}

std::uint32_t EditorApplication::save_generation() const {
    return active_buffer().save_generation;
}

std::uint32_t EditorApplication::save_generation(WindowId window) const {
    return state_for(buffer_id(window)).save_generation;
}

bool EditorApplication::has_background_work() const {
    return async_runtime_.has_work();
}

bool EditorApplication::poll_background_work() {
    return async_runtime_.drain() != 0;
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

void EditorApplication::destroy_window(WindowId window) {
    if (!runtime_.windows().erase(window)) {
        throw std::logic_error("window lifecycle registry is inconsistent");
    }
    for (auto iterator = views_.begin(); iterator != views_.end();) {
        if ((*iterator)->window != window) {
            ++iterator;
            continue;
        }
        const ViewId view = (*iterator)->view;
        iterator = views_.erase(iterator);
        if (!runtime_.views().erase(view)) {
            throw std::logic_error("view lifecycle registry is inconsistent");
        }
    }
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
    define("window.split-below",
           [this](const CommandInvocation&) { (void)split_window(WindowSplitAxis::Rows); });
    define("window.split-right",
           [this](const CommandInvocation&) { (void)split_window(WindowSplitAxis::Columns); });
    define("window.delete", [this](const CommandInvocation&) { (void)delete_window(); });
    define("window.delete-others", [this](const CommandInvocation&) { delete_other_windows(); });
    define("window.other", [this](const CommandInvocation&) { (void)select_other_window(); });
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

    project_find_file_accept_ = runtime_.commands().define(
        "project.find-file.accept",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept_project_find_file(context, invocation);
        });
    runtime_.commands().define(
        "project.find-file",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin_project_find_file(context, invocation);
        },
        [](const CommandContext& context) { return context.project_id().has_value(); });

    project_search_accept_ = runtime_.commands().define(
        "project.search.accept",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept_project_search(context, invocation);
        });
    runtime_.commands().define(
        "project.search",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin_project_search(context, invocation);
        },
        [](const CommandContext& context) { return context.project_id().has_value(); });

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
        "project-files", [](CommandContext& context, std::string_view) {
            std::vector<InteractionCandidate> candidates;
            const Project* project = context.project();
            if (project == nullptr || project->roots().empty()) {
                return candidates;
            }
            const fs::path root(project->roots().front());
            candidates.reserve(project->files().size());
            for (const std::string& file : project->files()) {
                fs::path relative = fs::path(file).lexically_relative(root);
                if (relative.empty()) {
                    relative = fs::path(file).filename();
                }
                candidates.push_back({.value = file,
                                      .label = relative.string(),
                                      .detail = relative.parent_path().string(),
                                      .filter_text = relative.string() + " " + file});
            }
            return candidates;
        });
    runtime_.interaction_providers().define(
        "files",
        [this](CommandContext&, std::string_view requested_query) -> InteractionProviderResult {
            const Buffer& buffer = session().buffer();
            std::optional<std::string> resource = buffer.resource_uri();
            std::string query(requested_query);
            return InteractionCandidateWork{[query = std::move(query),
                                             resource = std::move(resource)](
                                                const std::stop_token& cancellation) {
                const fs::path typed(query);
                fs::path directory;
                if (query.empty()) {
                    directory = resource ? fs::path(*resource).parent_path() : fs::path(".");
                } else if (query.ends_with(fs::path::preferred_separator)) {
                    directory = typed;
                } else {
                    directory = typed.has_parent_path() ? typed.parent_path() : fs::path(".");
                }
                std::expected<DirectoryListing, std::error_code> listing =
                    list_directory(directory, 1000, cancellation);
                if (!listing) {
                    if (listing.error() == std::make_error_code(std::errc::operation_canceled)) {
                        throw AsyncTaskCancelled();
                    }
                    return std::vector<InteractionCandidate>{};
                }

                std::vector<InteractionCandidate> candidates;
                candidates.reserve(listing->entries.size() + 1);
                if (listing->directory.has_parent_path()) {
                    const std::string parent = directory_input(listing->directory.parent_path());
                    candidates.push_back({.value = parent,
                                          .label = "../",
                                          .detail = listing->directory.parent_path().string(),
                                          .filter_text = parent});
                }
                for (const DirectoryEntry& entry : listing->entries) {
                    if (cancellation.stop_requested()) {
                        throw AsyncTaskCancelled();
                    }
                    std::string value = entry.path.string();
                    std::string label = entry.name;
                    if (entry.directory) {
                        value = directory_input(value);
                        label.push_back(fs::path::preferred_separator);
                    }
                    candidates.push_back({.value = value,
                                          .label = std::move(label),
                                          .detail = listing->directory.string(),
                                          .filter_text = value});
                }
                return candidates;
            }};
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
    } else if (result.status == CommandLoopStatus::Error ||
               result.status == CommandLoopStatus::Disabled ||
               result.status == CommandLoopStatus::Cancelled ||
               (result.status == CommandLoopStatus::NotHandled && result.consumed)) {
        // A prefix does not echo as a message: the pending sequence has its
        // own display channels (which-key title, echo right edge).
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
    const BufferId buffer = state.buffer;
    try {
        state.pending_save.emplace(PendingSave{.content = content, .task = {}});
        state.pending_save->task = async_runtime_.submit({
            .work = [this, buffer, path = std::move(target_path), content = std::move(content)](
                        const std::stop_token& cancellation) -> AsyncCompletion {
                std::error_code error;
                try {
                    error = save_file_atomically(path, content, cancellation);
                } catch (const std::system_error& exception) {
                    error = exception.code();
                } catch (const std::bad_alloc&) {
                    error = std::make_error_code(std::errc::not_enough_memory);
                } catch (...) {
                    error = std::make_error_code(std::errc::io_error);
                }
                if (error == std::make_error_code(std::errc::operation_canceled)) {
                    throw AsyncTaskCancelled();
                }
                return [this, buffer, error] { finish_save(buffer, error); };
            },
            .cancelled =
                [this, buffer] {
                    BufferState& cancelled = state_for(buffer);
                    cancelled.pending_save.reset();
                    message_ = "save cancelled";
                },
            .failed =
                [this, buffer](const std::exception_ptr& exception) {
                    std::error_code error = std::make_error_code(std::errc::io_error);
                    try {
                        if (exception) {
                            std::rethrow_exception(exception);
                        }
                    } catch (const std::system_error& system) {
                        error = system.code();
                    } catch (const std::bad_alloc&) {
                        error = std::make_error_code(std::errc::not_enough_memory);
                    } catch (...) {
                        error = std::make_error_code(std::errc::io_error);
                    }
                    finish_save(buffer, error);
                },
        });
        message_ = std::format("saving {}…", *resource);
    } catch (const std::system_error& exception) {
        state.pending_save.reset();
        message_ = std::format("save failed: {}", exception.code().message());
    } catch (const std::bad_alloc&) {
        state.pending_save.reset();
        message_ = "save failed: not enough memory";
    } catch (const std::exception& exception) {
        state.pending_save.reset();
        message_ = std::format("save failed: {}", exception.what());
    }
}

void EditorApplication::finish_save(BufferId buffer_id, std::error_code error) {
    BufferState& state = state_for(buffer_id);
    if (!state.pending_save) {
        return;
    }
    const Buffer& buffer = runtime_.buffers().get(buffer_id);
    const std::string display = buffer.resource_uri().value_or(buffer.name());
    if (error) {
        message_ = std::format("save failed: {}", error.message());
    } else {
        mark_saved(buffer_id, std::move(state.pending_save->content));
        message_ = buffer.modified() ? std::format("saved {} · newer edits remain", display)
                                     : std::format("saved {}", display);
    }
    state.pending_save.reset();
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
        buffer.resource_uri() ? fs::path(*buffer.resource_uri()).parent_path() : fs::path(".");
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
    if (input->ends_with(fs::path::preferred_separator)) {
        return InteractionRequest{.kind = InteractionKind::Picker,
                                  .prompt = "Open file: ",
                                  .initial_input = directory_input(*input),
                                  .history = "files",
                                  .provider = "files",
                                  .allow_custom_input = true,
                                  .accept_command = open_file_accept_,
                                  .arguments = {}};
    }
    std::expected<void, std::string> opened = open_file(*input);
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
        runtime_.projects().assign(context.buffer_id(),
                                   runtime_.projects().find_for_resource(*path));
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

CommandResult EditorApplication::begin_project_find_file(CommandContext& context,
                                                         const CommandInvocation&) {
    const std::optional<ProjectId> project = context.project_id();
    if (!project) {
        return std::unexpected(CommandError{"current buffer has no project"});
    }
    const Project& definition = runtime_.projects().get(*project);
    if (definition.index_revision() == 0 && !definition.indexing()) {
        project_service_->request_index(*project);
    }
    return InteractionRequest{.kind = InteractionKind::Picker,
                              .prompt = "Project file: ",
                              .initial_input = {},
                              .history = "project-files",
                              .provider = "project-files",
                              .allow_custom_input = false,
                              .accept_command = project_find_file_accept_,
                              .arguments = {}};
}

CommandResult EditorApplication::accept_project_find_file(CommandContext& context,
                                                          const CommandInvocation& invocation) {
    const std::string* path = submitted_string(invocation);
    if (path == nullptr) {
        return std::unexpected(CommandError{"project file picker requires a path"});
    }
    std::expected<void, std::string> opened = open_file(*path, context.window_id(), 0);
    return opened ? CommandResult{CommandCompleted{}}
                  : CommandResult{std::unexpected(CommandError{opened.error()})};
}

CommandResult EditorApplication::begin_project_search(CommandContext& context,
                                                      const CommandInvocation&) {
    if (!context.project_id()) {
        return std::unexpected(CommandError{"current buffer has no project"});
    }
    return InteractionRequest{.kind = InteractionKind::Text,
                              .prompt = "Project search: ",
                              .initial_input = {},
                              .history = "project-search",
                              .provider = {},
                              .allow_custom_input = true,
                              .accept_command = project_search_accept_,
                              .arguments = {}};
}

CommandResult EditorApplication::accept_project_search(CommandContext& context,
                                                       const CommandInvocation& invocation) {
    const std::string* query = submitted_string(invocation);
    const std::optional<ProjectId> project = context.project_id();
    if (query == nullptr || query->empty()) {
        return std::unexpected(CommandError{"project search query is empty"});
    }
    if (!project) {
        return std::unexpected(CommandError{"current buffer has no project"});
    }
    std::expected<void, std::string> started =
        start_project_search(*project, *query, context.window_id());
    return started ? CommandResult{CommandCompleted{}}
                   : CommandResult{std::unexpected(CommandError{started.error()})};
}

} // namespace cind
