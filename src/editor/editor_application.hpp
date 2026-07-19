#pragma once

#include "async/runtime.hpp"
#include "cli/session.hpp"
#include "editor/command_loop.hpp"
#include "editor/completion.hpp"
#include "editor/editing_mechanisms.hpp"
#include "editor/input_state.hpp"
#include "editor/interaction.hpp"
#include "editor/pointer.hpp"
#include "editor/project_service.hpp"
#include "editor/workbench.hpp"
#include "editor/workbench_session.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "lsp/registry.hpp"
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
    std::uint32_t initial_line = 0;
    EditorPlatformServices platform_services;
    std::optional<std::string> init_file;
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
    std::optional<std::string> role;
    bool pinned = false;
    bool created_by_policy = false;
    bool active = false;
};

struct WorkbenchLayoutSnapshot {
    std::optional<WindowId> window;
    WindowSplitAxis axis = WindowSplitAxis::Rows;
    float ratio = 0.5F;
    std::vector<WorkbenchLayoutSnapshot> children;
};

struct WorkbenchSnapshot {
    WorkbenchId workbench;
    std::string name;
    std::vector<ProjectId> scope;
    std::vector<BufferId> mru;
    std::vector<WindowId> windows;
    WindowId active_window;
    std::vector<std::pair<std::string, WindowId>> slots;
    WorkbenchLayoutSnapshot layout;
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
    ~EditorApplication();

    EditorRuntime& runtime() { return runtime_; }
    const EditorRuntime& runtime() const { return runtime_; }
    AsyncRuntime& async_runtime() { return async_runtime_; }
    const AsyncRuntime& async_runtime() const { return async_runtime_; }
    BufferId buffer_id() const;
    BufferId buffer_id(WindowId window) const;
    ViewId view_id() const;
    ViewId view_id(WindowId window) const;
    WindowId window_id() const { return workbenches_.active().active_window(); }
    WorkbenchId workbench_id() const { return workbenches_.active_id(); }
    EditSession& session();
    const EditSession& session() const;
    EditSession& session(WindowId window);
    const EditSession& session(WindowId window) const;
    const TokenBuffer& syntax_tokens() const;
    const TokenBuffer& syntax_tokens(WindowId window) const;
    const WindowLayout& window_layout() const { return workbenches_.active().layout(); }
    CommandLoop& command_loop() { return command_loop_; }
    const CommandLoop& command_loop() const { return command_loop_; }
    InteractionController& interaction() { return interaction_; }
    const InteractionController& interaction() const { return interaction_; }
    CompletionPipeline& completion() { return *completion_; }
    const CompletionPipeline& completion() const { return *completion_; }
    std::vector<LspSessionSnapshot> lsp_sessions() const { return lsp_sessions_->snapshots(); }
    GuileRuntimeSnapshot scripting() const { return guile_.snapshot(); }
    void refresh_default_keymap();

    bool handle_key(KeyStroke key, int page_rows);
    bool handle_pointer(const PointerEvent& event);
    bool handle_scroll(ScrollInput input);
    bool request_close(bool force = false);
    bool execute_command(std::string_view name, const CommandInvocation& invocation = {});
    std::expected<void, std::string> start_completion(CommandTarget target,
                                                      std::vector<CompletionProvider> providers,
                                                      CompletionTrigger trigger = {});
    bool move_completion(std::int64_t delta);
    std::expected<void, std::string> apply_completion(bool replace = false);
    bool cancel_completion();
    const InputStateRegistry::Definition& input_state() const;
    const InputStateRegistry::Definition& input_state(WindowId window) const;
    TextInputPolicy text_input_policy() const;
    void insert_text(std::string_view text);
    void reset_preferred_column();

    std::expected<void, std::string> open_file(std::string_view path);
    std::expected<WindowId, std::string> display_buffer(BufferId buffer, std::string_view intent,
                                                        WindowId origin);
    bool switch_buffer(BufferId buffer);
    bool focus_window(WindowId window);
    bool split_window(WindowSplitAxis axis);
    bool delete_window();
    bool delete_other_windows();
    std::expected<void, std::string> set_window_role(WindowId window,
                                                     std::optional<std::string> role);
    std::expected<void, std::string> set_window_pinned(WindowId window, bool pinned);
    std::expected<void, std::string> set_window_created_by_policy(WindowId window, bool created);
    std::optional<WindowId> workbench_slot(WorkbenchId workbench, std::string_view role) const;
    WorkbenchId create_workbench(std::string name, std::optional<ProjectId> project = std::nullopt);
    bool switch_workbench(WorkbenchId workbench);
    bool close_workbench(WorkbenchId workbench);
    bool adopt_project(WorkbenchId workbench, ProjectId project);
    bool expel_buffer(WorkbenchId workbench, BufferId buffer);
    std::vector<BufferId> workbench_buffers(WorkbenchId workbench, bool widen = false) const;
    std::vector<WorkbenchSnapshot> workbench_snapshots() const;
    WorkbenchSessionState capture_workbench_session() const;
    std::string serialize_workbench_session() const;
    std::expected<void, std::string> restore_workbench_session(std::string_view serialized);
    std::expected<void, std::string> release_buffer(BufferId buffer, BufferId replacement);
    std::vector<OpenBufferSnapshot> open_buffers() const;
    std::vector<OpenWindowSnapshot> open_windows() const;
    LocationNavigationSnapshot location_navigation() const;
    std::span<const KeymapLayer> active_keymap_layers() const {
        return command_loop_.keymap_layers();
    }
    std::vector<KeymapLayer> base_keymap_layers(WindowId window);
    std::string_view input_focus() const {
        return interaction_.active() ? std::string_view("minibuffer") : std::string_view("window");
    }
    std::string pending_key_sequence_text() const;
    std::string pending_prefix_text() const;
    std::string pending_input_state_name() const;
    std::vector<KeyBindingHint> pending_key_hints() const;
    PositionHintProviderResult position_hints(WindowId window);
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
    void set_message(std::string message) { message_ = std::move(message); }
    ChromeContent chrome_content(std::string_view preedit = {});
    ModelineContent modeline(WindowId window);
    const PresentationTheme& presentation_theme() const { return presentation_profile_.theme; }
    const PresentationStyleSheet& presentation_styles() const {
        return presentation_profile_.styles;
    }
    const PresentationMotion& presentation_motion() const { return presentation_profile_.motion; }
    const PresentationMetrics& presentation_metrics() const {
        return presentation_profile_.metrics;
    }
    const PresentationTypography& presentation_typography() const {
        return presentation_profile_.typography;
    }
    const std::string& last_key() const { return last_key_; }
    const std::string& last_command() const { return last_command_; }

    bool reveal_caret() const { return reveal_caret_; }
    void show_caret() { reveal_caret_ = true; }
    void hide_caret() { reveal_caret_ = false; }

    bool has_background_work() const;
    bool project_search_running() const { return guile_.project_search_running(); }
    bool poll_background_work();
    bool should_quit() const { return quit_; }

    void mark_saved(Text content);

private:
    struct PendingSave {
        Text content;
    };

    struct BufferState {
        BufferId buffer;
        std::shared_ptr<CppIndentStyle> style;
        std::string style_origin;
        std::uint32_t save_generation = 0;
        std::optional<PendingSave> pending_save;
    };

    struct LocationNavigationState {
        BufferId buffer;
        std::optional<std::size_t> selected_index;
    };

    struct ViewState {
        struct PositionHintCache {
            InputStateId input_state;
            RevisionId revision = 0;
            ViewSelection selection;
            EffectiveModePolicy mode_policy;
            PositionHintProviderResult result;
        };

        WindowId window;
        BufferId buffer;
        ViewId view;
        std::unique_ptr<EditSession> session;
        std::optional<PositionHintCache> position_hints;
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
    bool show_buffer(WindowId window, BufferId buffer);
    bool focus_window(WorkbenchId workbench, WindowId window);
    Workbench& active_workbench() { return workbenches_.active(); }
    const Workbench& active_workbench() const { return workbenches_.active(); }
    std::expected<WindowId, std::string> display_generated_buffer(WindowId origin, std::string name,
                                                                  std::string text, ModeId mode,
                                                                  std::string style_origin,
                                                                  std::string_view intent);
    std::expected<void, std::string> move_caret_to_line(ViewId view, std::uint32_t line,
                                                        std::uint32_t display_column);
    void scroll_view_lines(ViewId view, double lines);
    bool split_window(WindowId target, WindowSplitAxis axis);
    bool delete_window(WindowId target);
    bool delete_other_windows(WindowId retained);
    void destroy_window(WindowId window);
    void apply_position(WindowId window, LinePosition position);
    void register_commands();
    void register_input_states();
    void register_modes();
    void register_resource_policies();
    void register_buffer_lifecycle_policies();
    void register_pointer_policies();
    void register_interaction_providers();
    void register_keymaps();
    void register_presentation_policies();
    void sync_keymaps();
    const InputFeedback* active_input_feedback() const;
    bool handle_loop_result(CommandLoopResult result);
    bool execute_command(CommandId command, const CommandInvocation& invocation);
    void refresh_interaction_after_edit(RevisionId before);
    void refresh_completion_after_command();
    CompletionProviderResult dispatch_completion_provider(CompletionProvider provider,
                                                          const CompletionRequest& request);
    std::expected<CompletionProvider, std::string>
    resolve_completion_provider(CommandTarget target, std::string_view name);
    CommandContext command_context();
    void after_edit();
    std::expected<std::string, std::string> begin_buffer_save(BufferId buffer);
    std::expected<bool, std::string> complete_buffer_save(BufferId buffer);
    void abort_buffer_save(BufferId buffer);
    void mark_saved(BufferId buffer, Text content);
    EditorRuntime runtime_;
    GuileRuntime guile_;
    std::vector<std::unique_ptr<BufferState>> buffers_;
    std::vector<std::unique_ptr<ViewState>> views_;
    std::unique_ptr<ProjectService> project_service_;
    std::optional<LocationNavigationState> location_navigation_;
    WorkbenchRegistry workbenches_;
    InteractionController interaction_;
    std::unique_ptr<EditSession> interaction_session_;
    EditingMechanisms editing_mechanisms_;
    CommandLoop command_loop_;
    int command_page_rows_ = 1;
    std::string message_;
    std::string last_key_;
    std::string last_command_;
    PresentationProfile presentation_profile_;
    EditorPlatformServices platform_services_;
    bool reveal_caret_ = true;
    bool quit_ = false;
    std::uint64_t workbench_restore_generation_ = 0;
    // These are declared last so the script adapter first cancels its work,
    // then the native runtime joins before captured editor state is destroyed.
    AsyncRuntime async_runtime_;
    AsyncScriptHost script_async_;
    std::unique_ptr<LspSessionRegistry> lsp_sessions_;
    std::unique_ptr<CompletionPipeline> completion_;
};

} // namespace cind
