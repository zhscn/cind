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
enum class CommandLoopStatus : std::uint8_t;

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

struct GuileInteractionMechanismStatus {
    bool active = false;
    std::size_t candidate_count = 0;
    std::optional<BufferId> buffer;
    std::optional<ViewId> view;
    std::uint64_t candidate_revision = 0;
};

struct GuileInteractionPolicyState {
    InteractionKind kind = InteractionKind::Text;
    std::string keymap;
    std::string input_state;
    std::string buffer_name;
    std::string prompt;
    std::string history;
    bool allow_custom_input = false;
    std::string provider;
};

struct GuileMinibufferHistoryState {
    std::size_t entries = 0;
    std::optional<std::size_t> index;
    std::string draft;
};

struct GuileCommandFeedbackState {
    std::string message;
    std::string last_key;
    std::string last_command;
};

struct GuileViewPosition {
    std::uint32_t line = 0;
    std::uint32_t line_count = 0;
    std::uint32_t display_column = 0;
    std::uint32_t byte = 0;
    std::uint32_t byte_count = 0;
};

struct GuileViewLinePrefix {
    std::uint32_t line_start = 0;
    std::uint32_t caret = 0;
    std::string text;
};

struct GuileSyntaxToken {
    std::string kind;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

struct GuileLocationNavigation {
    std::optional<BufferId> buffer;
    std::optional<std::size_t> selected_index;
    std::size_t location_count = 0;
};

struct GuilePublishedLocationList {
    WorkbenchId workbench;
    std::uint64_t list = 0;
    BufferId buffer;
    std::size_t item_count = 0;
};

struct GuileLocationListPolicyState {
    std::uint64_t list = 0;
    std::optional<std::size_t> selected_index;
    bool current = false;
};

struct GuileLocationTarget {
    std::string resource;
    EncodedLinePosition position;
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

struct GuileWorkbenchRestoreTarget {
    WindowId window;
    std::uint32_t caret = 0;
};

struct GuileWorkbenchRestoreResource {
    std::string resource;
    std::vector<GuileWorkbenchRestoreTarget> targets;
};

struct GuileWorkbenchRestoreMru {
    WorkbenchId workbench;
    std::vector<std::string> resources;
    std::vector<WindowId> windows;
};

struct GuileWorkbenchRestorePlan {
    std::vector<GuileWorkbenchRestoreResource> resources;
    std::vector<GuileWorkbenchRestoreMru> mru;
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

struct GuileWorkbenchWindowState {
    WorkbenchId workbench;
    GuileDisplayWindow window;
};

// What the host was asked to create. The policy turns this into the buffer's
// name and keeps it; the host stores no name of its own.
struct GuileBufferIdentityFacts {
    std::string requested_name;
    BufferKind kind = BufferKind::Scratch;
    std::optional<std::string> resource;
};

struct GuileDisplayFacts {
    std::string intent;
    WindowId origin;
    WindowId active;
    // Layout order only. Role, pinning and slots are the policy's own state.
    std::vector<WindowId> windows;
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

struct GuileJumpNode {
    std::uint64_t id = 0;
    std::string resource;
    std::uint32_t line = 0;
    std::uint32_t byte_column = 0;
    std::string excerpt;
    std::uint64_t last_visit = 0;
};

struct GuileJumpWalkState {
    std::vector<std::uint64_t> entries;
    std::optional<std::size_t> cursor;
};

struct GuileDisplayPosition {
    EncodedLinePosition position;
};

struct GuileHostServices {
    std::function<std::expected<WindowId, std::string>(WindowId, BufferId, std::string_view,
                                                       std::optional<GuileDisplayPosition>)>
        display_buffer;
    std::function<std::expected<WindowId, std::string>(WindowId, std::string, std::string, ModeId,
                                                       std::string_view, std::string_view)>
        display_generated_buffer;
    std::function<bool(WindowId, std::int64_t)> navigate_jump;
    std::function<std::optional<std::uint64_t>(WindowId)> mark_jump;
    std::function<bool(WindowId, std::uint64_t)> visit_jump;
    std::function<bool(WindowId, std::uint64_t, std::uint64_t, std::string_view, bool)> link_jump;
    std::function<std::vector<GuileJumpEdge>(WindowId, bool)> jump_branches;
    std::function<std::optional<GuileJumpNode>(WindowId, std::uint64_t)> jump_node;
    std::function<std::size_t(WindowId, std::size_t)> evict_jumps;
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
    std::function<std::expected<void, std::string>(ViewId, std::string_view)> structural_edit;
    std::function<GuileInteractionMechanismStatus()> interaction_mechanism_status;
    std::function<std::optional<BufferId>()> interaction_origin_buffer;
    std::function<std::expected<void, std::string>(std::string_view)> refresh_interaction;
    std::function<std::expected<std::string, std::string>(std::optional<std::size_t>, bool)>
        submit_interaction;
    std::function<std::expected<RevisionId, std::string>(std::string_view)>
        replace_interaction_input;
    std::function<bool()> cancel_interaction;
    std::function<bool()> completion_active;
    std::function<std::expected<void, std::string>()> refresh_completion;
    std::function<std::expected<std::uint64_t, std::string>(CommandTarget, ScriptLspProviderSpec)>
        ensure_lsp_session;
    std::function<std::expected<void, std::string>(std::uint64_t)> attach_lsp_diagnostics;
    std::function<std::expected<void, std::string>(BufferId, std::uint64_t)>
        synchronize_lsp_session;
    std::function<std::expected<void, std::string>(CommandTarget, TextOffset,
                                                   std::vector<CompletionProvider>,
                                                   CompletionTrigger, CompletionSessionPolicy)>
        start_completion;
    std::function<bool(std::size_t)> focus_completion;
    std::function<std::expected<void, std::string>(std::size_t, bool)> apply_completion;
    std::function<bool()> cancel_completion;
    std::function<void()> cancel_pending_input;
    std::function<GuileViewPosition(ViewId)> view_position;
    std::function<GuileViewLinePrefix(ViewId)> view_line_prefix;
    std::function<std::optional<GuileSyntaxToken>(ViewId, TextOffset)> view_syntax_token;
    std::function<std::vector<std::string>(ViewId)> view_identifier_words;
    std::function<std::expected<GuilePublishedLocationList, std::string>(
        WindowId, BufferId, std::string, std::vector<BufferLocation>)>
        publish_location_list;
    std::function<std::optional<GuileLocationTarget>(WorkbenchId, std::uint64_t, std::size_t)>
        location_target;
    std::function<std::expected<void, std::string>(WindowId, BufferId, std::uint32_t)>
        position_buffer_view;
    std::function<std::expected<void, std::string>(ProjectId)> request_project_index;
    std::function<std::vector<BufferId>()> open_buffers;
    std::function<std::expected<WorkbenchId, std::string>(std::string, std::optional<ProjectId>)>
        create_workbench;
    std::function<std::expected<void, std::string>(WorkbenchId)> switch_workbench;
    std::function<std::expected<void, std::string>(WorkbenchId)> close_workbench;
    std::function<std::string()> workbench_session_state;
    std::function<std::expected<GuileWorkbenchRestorePlan, std::string>(std::string_view)>
        prepare_workbench_session_restore;
    std::function<std::expected<void, std::string>(WindowId, BufferId, std::uint32_t)>
        show_buffer_in_window;
    std::function<BufferId(WindowId)> window_buffer;
    std::function<std::expected<BufferId, std::string>(GuileBufferCreation)> create_buffer;
    std::function<std::expected<void, std::string>(BufferId, BufferId)> release_buffer;
    std::function<std::expected<void, std::string>(WindowId, WindowSplitAxis)> split_window;
    std::function<std::expected<void, std::string>(WindowId)> delete_window;
    std::function<std::expected<void, std::string>(WindowId)> delete_other_windows;
    std::function<std::vector<WindowId>()> open_windows;
    std::function<std::expected<void, std::string>(WindowId)> focus_window;
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

struct GuileApplicationState {
    bool exit_requested = false;
    bool reveal_caret = true;
    std::uint32_t page_rows = 1;
};

struct GuileCompletionDecision {
    std::optional<std::size_t> selection;
    bool cancel = false;
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
    std::expected<void, std::string> open_resource(
        WindowId window, std::string_view path, std::optional<std::uint32_t> line = std::nullopt,
        std::optional<std::uint32_t> column = std::nullopt, std::string_view intent = "edit");
    std::expected<void, std::string> restore_workbench_session(std::string_view serialized);
    std::expected<void, std::string> buffer_edited(BufferId buffer, ViewId view,
                                                   RevisionId revision);
    std::expected<void, std::string> interaction_started(const InteractionRequest& request,
                                                         CommandTarget origin);
    std::expected<GuileMinibufferHistoryState, std::string>
    minibuffer_history_state(BufferId buffer, std::string_view history) const;
    std::expected<std::optional<GuileInteractionPolicyState>, std::string>
    interaction_policy_state() const;
    std::expected<std::optional<std::size_t>, std::string> interaction_selection() const;
    std::expected<GuileCompletionDecision, std::string>
    completion_transition(const std::vector<std::uint64_t>& item_ids, bool automatic, bool pending);
    std::expected<void, std::string> completion_finished();
    std::expected<std::optional<std::size_t>, std::string> completion_selection() const;
    std::expected<GuileCommandFeedbackState, std::string> command_feedback_state() const;
    std::expected<CommandPrefix, std::string> command_prefix_state() const;
    std::expected<void, std::string> set_command_prefix_state(const CommandPrefix& prefix);
    std::expected<GuileApplicationState, std::string> application_state() const;
    std::expected<void, std::string> set_caret_reveal(bool reveal);
    std::expected<void, std::string> set_page_rows(std::uint32_t rows);
    std::expected<bool, std::string> lsp_session_bound(BufferId buffer,
                                                       std::uint64_t session) const;
    std::expected<void, std::string> buffer_created(BufferId buffer, std::string_view style_origin,
                                                    const GuileBufferIdentityFacts& identity);
    std::expected<std::string, std::string> buffer_name(BufferId buffer) const;
    std::expected<std::optional<ProjectId>, std::string> buffer_project(BufferId buffer) const;
    std::expected<std::optional<std::string>, std::string> buffer_resource(BufferId buffer) const;
    std::expected<BufferKind, std::string> buffer_kind(BufferId buffer) const;
    std::expected<std::optional<BufferId>, std::string>
    buffer_id_by_resource(std::string_view resource) const;
    std::expected<std::optional<BufferId>, std::string> buffer_id_by_name(std::string_view name);
    std::expected<std::string, std::string> buffer_style_origin(BufferId buffer) const;
    std::expected<void, std::string> buffer_released(BufferId buffer);
    std::expected<void, std::string> workbench_created(WorkbenchId workbench, std::string_view name,
                                                       WindowId root_window,
                                                       std::optional<BufferId> initial_buffer,
                                                       const std::vector<ProjectId>& scope);
    std::expected<WorkbenchId, std::string> active_workbench() const;
    std::expected<void, std::string> workbench_activate(WorkbenchId workbench);
    std::expected<WorkbenchId, std::string> workbench_next(WorkbenchId workbench,
                                                           int delta = 1) const;
    std::expected<WindowId, std::string> workbench_active_window(WorkbenchId workbench) const;
    std::expected<void, std::string> workbench_focus_window(WorkbenchId workbench, WindowId window);
    std::expected<void, std::string> workbench_visit_buffer(WorkbenchId workbench, BufferId buffer);
    std::expected<bool, std::string> workbench_expel_buffer(WorkbenchId workbench, BufferId buffer);
    std::expected<void, std::string> workbench_released(WorkbenchId workbench);
    std::expected<std::vector<BufferId>, std::string> workbench_mru(WorkbenchId workbench) const;
    std::expected<std::vector<BufferId>, std::string> workbench_buffers(WorkbenchId workbench,
                                                                        bool widen = false) const;
    std::expected<GuileLocationNavigation, std::string>
    workbench_location_navigation(WorkbenchId workbench) const;
    std::expected<std::vector<GuileLocationListPolicyState>, std::string>
    workbench_location_list_states(WorkbenchId workbench) const;
    std::expected<void, std::string> workbench_transaction_group_recorded(WorkbenchId workbench,
                                                                          std::uint64_t group);
    std::expected<bool, std::string> workbench_transaction_group_movable(WorkbenchId workbench,
                                                                         std::uint64_t group,
                                                                         bool redo) const;
    std::expected<void, std::string> workbench_transaction_group_moved(WorkbenchId workbench,
                                                                       std::uint64_t group,
                                                                       bool redo, bool changed);
    std::expected<bool, std::string> workbench_jump_record(WindowId window, std::uint64_t node);
    std::expected<std::optional<std::uint64_t>, std::string>
    workbench_jump_move(WindowId window, std::int64_t delta);
    std::expected<std::optional<std::uint64_t>, std::string>
    workbench_jump_current(WindowId window) const;
    std::expected<void, std::string> workbench_jump_forget(WorkbenchId workbench,
                                                           const std::vector<std::uint64_t>& nodes);
    std::expected<void, std::string>
    workbench_jump_restore(WindowId window, const std::vector<std::uint64_t>& entries,
                           std::optional<std::size_t> cursor);
    std::expected<GuileJumpWalkState, std::string> workbench_jump_walk(WindowId window) const;
    std::expected<GuileJumpWalkState, std::string>
    workbench_jump_session_walk(WindowId window, const std::vector<std::uint64_t>& durable_nodes,
                                std::size_t maximum_entries) const;
    std::expected<bool, std::string> workbench_jump_track_intent(std::string_view intent) const;
    std::expected<std::optional<std::string>, std::string>
    workbench_jump_transition(WindowId window, std::string_view intent, std::uint64_t from,
                              std::uint64_t to);
    std::expected<void, std::string> replace_workbench_mru(WorkbenchId workbench,
                                                           const std::vector<BufferId>& buffers);
    std::expected<std::string, std::string> workbench_name(WorkbenchId workbench) const;
    std::expected<std::optional<WorkbenchId>, std::string>
    workbench_find_by_name(std::string_view name) const;
    std::expected<bool, std::string> workbench_rename(WorkbenchId workbench, std::string_view name);
    std::expected<std::vector<ProjectId>, std::string> workbench_scope(WorkbenchId workbench) const;
    std::expected<bool, std::string> workbench_adopt_project(WorkbenchId workbench,
                                                             ProjectId project);
    std::expected<void, std::string> workbench_window_added(WorkbenchId workbench, WindowId window);
    std::expected<bool, std::string> workbench_forget_window(WindowId window);
    std::expected<GuileWorkbenchWindowState, std::string>
    workbench_window_state(WindowId window) const;
    std::expected<void, std::string>
    workbench_set_window_role(WindowId window, std::optional<std::string_view> role);
    std::expected<void, std::string> workbench_set_window_pinned(WindowId window, bool pinned);
    std::expected<void, std::string> workbench_set_window_created_by_policy(WindowId window,
                                                                            bool created);
    std::expected<std::optional<WindowId>, std::string> workbench_slot(WorkbenchId workbench,
                                                                       std::string_view role) const;
    std::expected<std::vector<GuileDisplaySlot>, std::string>
    workbench_slots(WorkbenchId workbench) const;
    std::expected<void, std::string> lsp_diagnostics_failed(std::string_view message);
    std::expected<bool, std::string> buffer_saving(BufferId buffer) const;
    std::expected<void, std::string> command_input(std::string_view key, bool clear_message);
    std::expected<void, std::string>
    command_result_feedback(CommandLoopStatus status, bool consumed,
                            std::optional<std::string_view> command, bool interaction_started,
                            std::string_view message);
    std::expected<void, std::string> record_command(std::string_view command);
    std::expected<void, std::string> set_message(std::string_view message);
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
    std::expected<GuileDisplayPlan, std::string>
    fallback_display_plan(const GuileDisplayFacts& facts) const;
    std::expected<void, std::string> load_extension(const std::string& path);
    std::expected<GuileEvaluationResult, std::string> evaluate(GuileEvaluationRequest request);
    std::expected<CompletionProviderResult, std::string>
    complete(const CompletionProvider& provider, const CompletionRequest& request);
    std::expected<CompletionItem, std::string> resolve(const CompletionProvider& provider,
                                                       const CompletionRequest& request,
                                                       const CompletionItem& item);
    GuileRuntimeSnapshot snapshot() const;

    // Releases task callbacks while the application's async service is still
    // alive. Safe to call repeatedly during owner-thread shutdown.
    void shutdown_async_tasks() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind
