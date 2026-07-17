#pragma once

#include "async/runtime.hpp"
#include "cli/session.hpp"
#include "editor/basic_commands.hpp"
#include "editor/command_loop.hpp"
#include "editor/input_state.hpp"
#include "editor/interaction.hpp"
#include "editor/location_list_mode.hpp"
#include "editor/project_service.hpp"
#include "editor/search_commands.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "script/guile_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cind {

struct LocationListDocument;

struct EditorPlatformServices {
    std::function<std::expected<void, std::string>(std::string_view)> write_clipboard;
    std::function<std::expected<std::string, std::string>()> read_clipboard;
    // Thread-safe native event-loop notification. The callback only wakes the
    // frontend; editor state is applied later by poll_background_work().
    AsyncRuntime::Wakeup wake_event_loop;
};

struct EditorApplicationSpec {
    std::string path;
    // An omitted value asks the application to load path through its async
    // runtime. An engaged empty string represents a preloaded empty file.
    std::optional<std::string> initial_text;
    CppIndentStyle style;
    std::string style_origin;
    std::uint32_t initial_line = 0;
    EditorPlatformServices platform_services;
};

struct OpenBufferSnapshot {
    BufferId buffer;
    std::optional<ViewId> view;
    std::string name;
    std::optional<std::string> resource;
    bool modified = false;
    bool active = false;
    bool saving = false;
    std::string major_mode;
    std::string interaction_class;
    std::string initial_input_state;
    std::vector<ModeThingBinding> things;
    std::size_t location_count = 0;
};

struct OpenWindowSnapshot {
    WindowId window;
    ViewId view;
    BufferId buffer;
    bool active = false;
};

struct LocationNavigationSnapshot {
    std::optional<BufferId> buffer;
    std::optional<std::size_t> selected_index;
    std::size_t location_count = 0;
};

using KeyBindingHint = InputHint;

// Frontend-independent state and command controller for one editor
// application. Frontends translate native events into KeyStroke/text input
// and render the exposed document, interaction and view state.
class EditorApplication {
public:
    explicit EditorApplication(EditorApplicationSpec spec);

    EditorRuntime& runtime() { return runtime_; }
    const EditorRuntime& runtime() const { return runtime_; }
    AsyncRuntime& async_runtime() { return async_runtime_; }
    const AsyncRuntime& async_runtime() const { return async_runtime_; }
    BufferId buffer_id() const;
    BufferId buffer_id(WindowId window) const;
    ViewId view_id() const;
    ViewId view_id(WindowId window) const;
    WindowId window_id() const { return active_window_; }
    EditSession& session();
    const EditSession& session() const;
    EditSession& session(WindowId window);
    const EditSession& session(WindowId window) const;
    const TokenBuffer& syntax_tokens() const;
    const TokenBuffer& syntax_tokens(WindowId window) const;
    const WindowLayout& window_layout() const { return window_layout_; }
    SearchCommands& search_commands() { return search_commands_; }
    const SearchCommands& search_commands() const { return search_commands_; }
    CommandLoop& command_loop() { return command_loop_; }
    const CommandLoop& command_loop() const { return command_loop_; }
    InteractionController& interaction() { return interaction_; }
    const InteractionController& interaction() const { return interaction_; }
    GuileRuntimeSnapshot scripting() const { return guile_.snapshot(); }
    KeymapId default_keymap() const { return keymap_; }

    void refresh_default_keymap();

    bool handle_key(KeyStroke key, int page_rows);
    const InputStateRegistry::Definition& input_state() const;
    const InputStateRegistry::Definition& input_state(WindowId window) const;
    TextInputPolicy text_input_policy() const;
    void insert_text(std::string_view text);
    void reset_preferred_column();

    std::expected<void, std::string> open_file(std::string_view path);
    bool switch_buffer(BufferId buffer);
    bool focus_window(WindowId window);
    bool split_window(WindowSplitAxis axis);
    bool delete_window();
    void delete_other_windows();
    bool select_other_window(int delta = 1);
    std::expected<void, std::string> kill_buffer(BufferId buffer, bool force = false);
    std::vector<OpenBufferSnapshot> open_buffers() const;
    std::vector<OpenWindowSnapshot> open_windows() const;
    LocationNavigationSnapshot location_navigation() const;
    std::span<const KeymapLayer> active_keymap_layers() const {
        return command_loop_.keymap_layers();
    }
    std::vector<KeymapLayer> base_keymap_layers(WindowId window) const;
    std::string_view input_focus() const {
        return interaction_.active() ? std::string_view("interaction") : std::string_view("window");
    }
    std::string pending_key_sequence_text() const;
    std::string pending_prefix_text() const;
    std::string pending_command_text() const;
    std::string pending_input_state_name() const;
    std::vector<KeyBindingHint> pending_key_hints() const;
    std::size_t buffer_count() const { return buffers_.size(); }

    RevisionId revision() const { return session().snapshot().revision(); }
    RevisionId revision(WindowId window) const { return session(window).snapshot().revision(); }
    bool dirty() const { return session().buffer().modified(); }
    bool dirty(WindowId window) const { return session(window).buffer().modified(); }
    const std::string& path() const;
    const std::string& path(WindowId window) const;
    const std::string& style_origin() const;
    const std::string& style_origin(WindowId window) const;
    std::uint32_t save_generation() const;
    std::uint32_t save_generation(WindowId window) const;

    const std::string& message() const { return message_; }
    std::string& message() { return message_; }
    void set_message(std::string message) { message_ = std::move(message); }
    const std::string& last_key() const { return last_key_; }
    const std::string& last_command() const { return last_command_; }

    bool reveal_caret() const { return reveal_caret_; }
    void show_caret() { reveal_caret_ = true; }
    void hide_caret() { reveal_caret_ = false; }

    bool has_background_work() const;
    bool project_search_running() const {
        return project_search_.process.valid() || project_search_.parse_task.valid();
    }
    bool poll_background_work();
    bool should_quit() const { return quit_; }
    bool quit_armed() const { return quit_armed_; }
    void request_quit(bool force = false);

    void mark_saved(Text content);

private:
    struct PendingSave {
        Text content;
        AsyncTaskId task;
    };

    struct PendingOpen {
        std::string resource;
        WindowId target_window;
        std::optional<LinePosition> position;
        AsyncTaskId task;
    };

    struct BufferState {
        BufferId buffer;
        std::shared_ptr<CppIndentStyle> style;
        std::string style_origin;
        std::uint32_t save_generation = 0;
        std::optional<PendingSave> pending_save;
    };

    struct ProjectSearchState {
        std::uint64_t generation = 0;
        AsyncProcessId process;
        AsyncTaskId parse_task;
    };

    struct LocationNavigationState {
        BufferId buffer;
        std::optional<std::size_t> selected_index;
    };

    struct ViewState {
        WindowId window;
        BufferId buffer;
        ViewId view;
        std::unique_ptr<EditSession> session;
    };

    BufferState& active_buffer();
    const BufferState& active_buffer() const;
    BufferState& state_for(BufferId buffer);
    const BufferState& state_for(BufferId buffer) const;
    ViewState& active_view();
    const ViewState& active_view() const;
    ViewState& view_state_for(ViewId view);
    const ViewState& view_state_for(ViewId view) const;
    ViewState* find_view(WindowId window, BufferId buffer);
    const ViewState* find_view(WindowId window, BufferId buffer) const;
    EditSession& session_for(ViewId view);
    const EditSession& session_for(ViewId view) const;
    BufferId create_buffer(BufferSpec spec, CppIndentStyle style, std::string style_origin,
                           std::optional<ModeId> major_mode, TextOffset caret = {});
    ViewId create_view(WindowId window, BufferId buffer, TextOffset caret = {});
    BufferId create_scratch_buffer();
    bool show_buffer(WindowId window, BufferId buffer);
    std::expected<void, std::string> move_caret_to_line(ViewId view, std::uint32_t line,
                                                        std::uint32_t display_column);
    bool split_window(WindowId target, WindowSplitAxis axis);
    bool delete_window(WindowId target);
    void delete_other_windows(WindowId retained);
    bool select_other_window(WindowId from, int delta);
    void destroy_window(WindowId window);
    std::expected<void, std::string> start_project_search(ProjectId project, std::string query,
                                                          WindowId target_window);
    void finish_project_search(ProjectId project, WindowId target_window, std::uint64_t generation,
                               std::string query, AsyncProcessResult result);
    void finish_project_search_document(ProjectId project, WindowId target_window,
                                        std::uint64_t generation, std::string query,
                                        LocationListDocument document);
    void fail_project_search(std::uint64_t generation, const std::exception_ptr& failure);
    void cancel_project_search(std::uint64_t generation);
    void apply_position(WindowId window, LinePosition position);
    CommandResult visit_location(CommandContext& context);
    CommandResult move_location(CommandContext& context, int direction);
    CommandResult navigate_location(CommandContext& context, int direction);
    CommandResult visit_location_at(BufferId list, std::size_t index, WindowId target_window);
    bool location_navigation_available(const CommandContext& context) const;

    void register_commands();
    void register_input_states();
    void register_modes();
    void register_interaction_providers();
    void register_keymaps();
    void sync_keymaps();
    std::vector<KeymapLayer> window_keymap_layers() const;
    const InputFeedback* active_input_feedback() const;
    bool handle_loop_result(CommandLoopResult result);
    CommandContext command_context();
    void after_edit();
    void save(BufferId buffer);
    std::expected<void, std::string> open_file(std::string_view path, WindowId target_window,
                                               std::optional<LinePosition> position);
    void finish_open(std::string resource, std::string contents, CppIndentStyle style,
                     std::string style_origin, const std::optional<ProjectDiscovery>& project);
    void fail_open(std::string_view resource, const std::exception_ptr& failure);
    void cancel_open(std::string_view resource);
    void finish_save(BufferId buffer, std::error_code error);
    void mark_saved(BufferId buffer, Text content);

    EditorRuntime runtime_;
    GuileRuntime guile_;
    std::vector<std::unique_ptr<BufferState>> buffers_;
    std::vector<std::unique_ptr<ViewState>> views_;
    std::unique_ptr<ProjectService> project_service_;
    std::vector<PendingOpen> pending_opens_;
    std::optional<BufferId> startup_placeholder_;
    ProjectSearchState project_search_;
    std::optional<LocationNavigationState> location_navigation_;
    WindowId active_window_;
    WindowLayout window_layout_;
    InteractionController interaction_;
    BasicEditorCommands basic_commands_;
    SearchCommands search_commands_;
    CommandLoop command_loop_;
    KeymapId keymap_;
    KeymapId application_keymap_;
    KeymapId system_keymap_;
    KeymapId interaction_text_keymap_;
    KeymapId interaction_picker_keymap_;
    ModeId cpp_mode_;
    ModeId location_list_mode_;
    int command_page_rows_ = 1;
    std::string message_;
    std::string last_key_;
    std::string last_command_;
    EditorPlatformServices platform_services_;
    bool reveal_caret_ = true;
    bool quit_armed_ = false;
    bool quit_ = false;
    // Declared last so shutdown cancels and joins background work before any
    // editor state captured by completion callbacks is destroyed.
    AsyncRuntime async_runtime_;
};

} // namespace cind
