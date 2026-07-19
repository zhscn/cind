#pragma once

#include "editor/buffer.hpp"
#include "editor/command.hpp"
#include "editor/completion.hpp"
#include "editor/ids.hpp"
#include "editor/pointer.hpp"
#include "editor/selection.hpp"
#include "editor/startup.hpp"
#include "editor/window.hpp"
#include "presentation/chrome.hpp"
#include "presentation/modeline.hpp"
#include "presentation/profile.hpp"
#include "script/async_host.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

class EditorRuntime;

std::optional<std::string> discover_user_init_file();

struct GuileKeyBindingSummary {
    std::string keys;
    std::string command;
};

struct GuileKeymapLayer {
    KeymapId keymap;
    std::string scope;

    friend bool operator==(const GuileKeymapLayer&, const GuileKeymapLayer&) = default;
};

struct GuileKeymapPolicy {
    std::vector<GuileKeymapLayer> layers;
    std::vector<KeymapId> overrides;

    friend bool operator==(const GuileKeymapPolicy&, const GuileKeymapPolicy&) = default;
};

struct GuileTextRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

struct GuileInteractionStatus {
    bool active = false;
    bool picker = false;
    bool has_history = false;
    std::optional<std::string> history;
    std::optional<std::size_t> selected;
    std::size_t candidate_count = 0;
    std::optional<std::size_t> history_index;
    std::string history_draft;
};

struct GuileInteractionSubmission {
    CommandDispatch dispatch;
    std::string history;
};

struct GuileViewPosition {
    std::uint32_t line = 0;
    std::uint32_t line_count = 0;
    std::uint32_t display_column = 0;
    std::uint32_t byte = 0;
    std::uint32_t byte_count = 0;
};

struct GuileLocationNavigation {
    std::optional<BufferId> buffer;
    std::optional<std::size_t> selected_index;
    std::size_t location_count = 0;
};

struct GuileLocationTarget {
    std::string resource;
    LinePosition position;
    bool stale = false;
};

struct GuileBufferCreation {
    std::string name;
    std::string initial_text;
    BufferKind kind = BufferKind::Scratch;
    std::optional<std::string> resource;
    bool read_only = false;
    std::optional<ModeId> major_mode;
    CppIndentStyle style;
    std::string style_origin;
};

struct GuileWorkbenchSummary {
    WorkbenchId workbench;
    std::string name;
    std::vector<ProjectId> scope;
    std::vector<BufferId> mru;
    bool active = false;
};

struct GuileDisplayWindow {
    WindowId window;
    std::optional<std::string> role;
    bool pinned = false;
    bool created_by_policy = false;
};

struct GuileDisplaySlot {
    std::string role;
    WindowId window;
};

struct GuileDisplayFacts {
    std::string intent;
    WindowId origin;
    WindowId active;
    std::vector<GuileDisplayWindow> windows;
    std::vector<GuileDisplaySlot> slots;
};

struct GuileDisplayPlan {
    enum class Action : std::uint8_t { Reuse, Split };

    Action action = Action::Reuse;
    WindowId target;
    WindowSplitAxis axis = WindowSplitAxis::Columns;
    float ratio = 0.5F;
    std::optional<std::string> role;
};

enum class GuileDeleteOutcome : std::uint8_t {
    Unchanged,
    Deleted,
    MovedOverPair,
    MovedOverLiteral,
};

struct GuileJumpEdge {
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    std::string kind;
    std::uint64_t at = 0;
    bool persistent = false;
};

struct GuileDisplayPosition {
    std::uint32_t line = 0;
    std::uint32_t byte_column = 0;
};

struct GuileHostServices {
    std::function<std::expected<WindowId, std::string>(WindowId, BufferId, std::string_view,
                                                       std::optional<GuileDisplayPosition>)>
        display_buffer;
    std::function<std::expected<WindowId, std::string>(WindowId, std::string, std::string, ModeId,
                                                       std::string, std::string_view)>
        display_generated_buffer;
    std::function<bool(WindowId, std::int64_t)> navigate_jump;
    std::function<std::optional<std::uint64_t>(WindowId)> mark_jump;
    std::function<bool(WindowId, std::uint64_t)> visit_jump;
    std::function<bool(WindowId, std::uint64_t, std::uint64_t, std::string_view, bool)> link_jump;
    std::function<std::vector<GuileJumpEdge>(WindowId, bool)> jump_branches;
    std::function<std::expected<void, std::string>(ViewId, std::uint32_t, std::uint32_t)>
        move_caret_to_line;
    std::function<bool(ViewId)> undo;
    std::function<bool(ViewId)> redo;
    std::function<void(ViewId, std::uint32_t)> set_view_caret;
    std::function<void(ViewId, std::int64_t)> move_caret_lines;
    std::function<void(ViewId, double)> scroll_view_lines;
    std::function<void(ViewId, bool)> move_caret_line_boundary;
    std::function<GuileDeleteOutcome(ViewId, bool, bool)> delete_grapheme;
    std::function<void(ViewId)> newline;
    std::function<std::optional<std::string>(ViewId)> indent;
    std::function<std::expected<void, std::string>(ViewId, std::string_view)> type_text;
    std::function<int()> page_rows;
    std::function<GuileInteractionStatus()> interaction_status;
    std::function<std::optional<std::string>()> interaction_provider;
    std::function<std::expected<void, std::string>(std::string)> set_interaction_provider;
    std::function<std::optional<ProjectId>()> interaction_origin_project;
    std::function<void()> refresh_interaction;
    std::function<std::expected<GuileInteractionSubmission, std::string>()> submit_interaction;
    std::function<std::vector<std::string>(std::string_view)> interaction_history;
    std::function<void(std::string, std::vector<std::string>)> set_interaction_history;
    std::function<bool(std::size_t)> select_interaction_candidate;
    std::function<bool(std::optional<std::size_t>, std::string, std::string)>
        set_interaction_history_position;
    std::function<bool()> cancel_interaction;
    std::function<bool()> completion_active;
    std::function<std::expected<CompletionProvider, std::string>(CommandTarget, std::string_view)>
        resolve_completion_provider;
    std::function<std::expected<void, std::string>(CommandTarget, std::vector<CompletionProvider>)>
        start_completion;
    std::function<bool(std::int64_t)> move_completion;
    std::function<std::expected<void, std::string>(bool)> apply_completion;
    std::function<bool()> cancel_completion;
    std::function<void()> cancel_pending_input;
    std::function<GuileViewPosition(ViewId)> view_position;
    std::function<std::expected<void, std::string>(WindowId, BufferId, std::string,
                                                   std::vector<BufferLocation>)>
        publish_location_list;
    std::function<GuileLocationNavigation()> location_navigation;
    std::function<std::expected<void, std::string>(std::optional<BufferId>,
                                                   std::optional<std::size_t>)>
        set_location_navigation;
    std::function<std::optional<GuileLocationTarget>(std::optional<BufferId>, std::size_t)>
        location_target;
    std::function<bool(int)> move_location_list;
    std::function<std::expected<void, std::string>(WindowId, BufferId, std::uint32_t)>
        position_buffer_view;
    std::function<void(std::string)> set_message;
    std::function<std::expected<void, std::string>(ProjectId)> request_project_index;
    std::function<std::expected<std::string, std::string>(BufferId)> begin_buffer_save;
    std::function<std::expected<bool, std::string>(BufferId)> complete_buffer_save;
    std::function<void(BufferId)> abort_buffer_save;
    std::function<std::vector<BufferId>()> open_buffers;
    std::function<std::vector<GuileWorkbenchSummary>()> workbenches;
    std::function<WorkbenchId()> active_workbench;
    std::function<std::vector<BufferId>(WorkbenchId, bool)> workbench_buffers;
    std::function<std::expected<WorkbenchId, std::string>(std::string, std::optional<ProjectId>)>
        create_workbench;
    std::function<std::expected<void, std::string>(WorkbenchId)> switch_workbench;
    std::function<std::expected<void, std::string>(WorkbenchId)> close_workbench;
    std::function<std::expected<void, std::string>(WorkbenchId, ProjectId)> adopt_project;
    std::function<std::expected<void, std::string>(WorkbenchId, BufferId)> expel_buffer;
    std::function<std::string()> workbench_session_state;
    std::function<std::expected<void, std::string>(std::string_view)> restore_workbench_session;
    std::function<std::expected<BufferId, std::string>(GuileBufferCreation)> create_buffer;
    std::function<bool(BufferId)> buffer_saving;
    std::function<std::expected<void, std::string>(BufferId, BufferId)> release_buffer;
    std::function<void()> request_exit;
    std::function<std::expected<void, std::string>(WindowId, WindowSplitAxis)> split_window;
    std::function<std::expected<void, std::string>(WindowId)> delete_window;
    std::function<std::expected<void, std::string>(WindowId)> delete_other_windows;
    std::function<std::vector<WindowId>()> open_windows;
    std::function<WindowId()> active_window;
    std::function<std::expected<void, std::string>(WindowId, std::optional<std::string>)>
        set_window_role;
    std::function<std::expected<void, std::string>(WindowId, bool)> set_window_pinned;
    std::function<std::optional<WindowId>(WorkbenchId, std::string_view)> workbench_slot;
    std::function<std::expected<void, std::string>(WindowId)> focus_window;
    std::function<void()> request_redraw;
    std::function<void(bool)> set_caret_reveal;
    std::function<std::vector<GuileKeyBindingSummary>()> active_key_bindings;
    std::function<void(ViewId, ViewSelection)> set_selection;
    std::function<void(ViewId)> clear_selection;
    std::function<std::expected<ViewSelection, std::string>(ViewId, ViewSelection,
                                                            std::vector<std::string>)>
        replace_selection;
    std::function<std::expected<std::vector<std::string>, std::string>(ViewId,
                                                                       const ViewSelection&)>
        selection_texts;
    std::function<std::expected<void, std::string>(ViewId, GuileTextRange)> erase_range;
    std::function<std::expected<void, std::string>(ViewId, std::vector<std::string>)> insert_text;
    std::function<std::expected<std::optional<GuileTextRange>, std::string>(ViewId, bool)>
        soft_kill_range;
    std::function<std::expected<std::optional<ViewSelection>, std::string>(
        ViewId, const ViewSelection&, std::string_view, bool)>
        thing_selection;
    std::function<std::expected<ViewSelection, std::string>(ViewId, const ViewSelection&,
                                                            std::string_view, std::int64_t, bool)>
        motion_selection;
    std::function<std::expected<std::optional<ViewSelection>, std::string>(ViewId,
                                                                           const ViewSelection&)>
        expand_selection;
    std::function<std::expected<void, std::string>(std::string_view)> write_clipboard;
    std::function<std::expected<std::optional<std::string>, std::string>()> read_clipboard;
    std::function<std::expected<std::uint64_t, std::string>(ScriptAsyncRequest,
                                                            ScriptAsyncCallbacks)>
        start_async_task;
    std::function<bool(std::uint64_t)> cancel_async_task;
    std::function<std::vector<ScriptAsyncTaskSummary>()> async_tasks;
};

struct GuileEvaluationResult {
    std::vector<std::string> values;
    std::string output;
    std::string error_output;
    std::optional<std::string> error;
};

struct GuileEvaluationRequest {
    std::string_view source;
    std::string_view source_name;
};

struct GuileRuntimeSnapshot {
    std::string engine;
    std::string version;
    std::vector<std::string> modules;
    std::vector<std::string> extensions;
    std::uint64_t command_revision = 0;
    std::size_t scripted_commands = 0;
    std::uint64_t provider_revision = 0;
    std::size_t scripted_providers = 0;
    std::uint64_t binding_revision = 0;
    std::uint64_t input_state_revision = 0;
    std::size_t scripted_input_states = 0;
    std::size_t scripted_input_strategies = 0;
    std::uint64_t mode_revision = 0;
    std::size_t scripted_modes = 0;
    std::uint64_t resource_policy_revision = 0;
    std::size_t scripted_file_mode_rules = 0;
    std::size_t scripted_project_providers = 0;
    std::size_t outstanding_async_tasks = 0;
    std::optional<std::string> last_error;
};

// Owns the editor-thread Guile policy environment. C++ registries and
// generational editor objects remain authoritative; Scheme receives only
// explicit host capabilities and never a process-global current editor.
class GuileRuntime {
public:
    explicit GuileRuntime(EditorRuntime& runtime, GuileHostServices services = {});
    ~GuileRuntime();

    GuileRuntime(const GuileRuntime&) = delete;
    GuileRuntime& operator=(const GuileRuntime&) = delete;

    std::expected<std::size_t, std::string> install_core_commands();
    std::expected<std::size_t, std::string> install_core_providers();
    std::expected<std::size_t, std::string> install_default_keymaps();
    std::expected<std::size_t, std::string> install_input_states();
    std::expected<std::size_t, std::string> install_core_modes();
    std::expected<std::size_t, std::string> install_core_resource_policies();
    std::expected<void, std::string> install_buffer_lifecycle_policies();
    std::expected<void, std::string> install_pointer_policies();
    std::expected<void, std::string> install_presentation_policies();
    std::expected<void, std::string> install_display_policy();
    std::expected<StartupPlan, std::string> startup_plan(const StartupFacts& facts) const;
    std::expected<SessionPlan, std::string> session_plan(const SessionFacts& facts) const;
    std::expected<void, std::string> set_startup_placeholder(std::optional<BufferId> buffer);
    std::expected<CommandId, std::string> close_command(const CommandContext& context,
                                                        bool force) const;
    std::expected<bool, std::string> handle_pointer(const CommandContext& context,
                                                    const PointerEvent& event,
                                                    bool pending_key_sequence) const;
    std::expected<bool, std::string> handle_scroll(const CommandContext& context,
                                                   ScrollInput input) const;
    std::expected<void, std::string>
    open_resource(WindowId window, std::string_view path,
                  std::optional<std::uint32_t> line = std::nullopt,
                  std::optional<std::uint32_t> column = std::nullopt);
    bool project_search_running() const;
    void project_index_updated(ProjectId project);
    std::expected<GuileKeymapPolicy, std::string>
    keymap_policy(const CommandContext& context) const;
    std::expected<GuileKeymapPolicy, std::string>
    base_keymap_policy(const CommandContext& context) const;
    std::expected<ChromeContent, std::string> chrome_content(const CommandContext& context,
                                                             const ChromeFacts& facts) const;
    std::expected<ModelineContent, std::string> modeline_content(const CommandContext& context,
                                                                 const ModelineFacts& facts) const;
    std::expected<PresentationProfile, std::string> presentation_profile() const;
    std::expected<GuileDisplayPlan, std::string> display_plan(const GuileDisplayFacts& facts) const;
    std::expected<void, std::string> load_extension(const std::string& path);
    std::expected<GuileEvaluationResult, std::string> evaluate(GuileEvaluationRequest request);
    GuileRuntimeSnapshot snapshot() const;

    // Releases task callbacks while the application's async service is still
    // alive. Safe to call repeatedly during owner-thread shutdown.
    void shutdown_async_tasks() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind
