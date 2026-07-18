#pragma once

#include "editor/buffer.hpp"
#include "editor/command.hpp"
#include "editor/ids.hpp"
#include "editor/pointer.hpp"
#include "editor/selection.hpp"
#include "editor/startup.hpp"
#include "editor/window.hpp"
#include "presentation/modeline.hpp"
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

enum class GuileDeleteOutcome : std::uint8_t {
    Unchanged,
    Deleted,
    MovedOverPair,
    MovedOverLiteral,
};

struct GuileHostServices {
    std::function<std::expected<void, std::string>(WindowId, BufferId)> display_buffer;
    std::function<std::expected<void, std::string>(WindowId, std::string, std::string)>
        display_generated_buffer;
    std::function<std::expected<void, std::string>(ViewId, std::uint32_t, std::uint32_t)>
        move_caret_to_line;
    std::function<bool(ViewId)> undo;
    std::function<bool(ViewId)> redo;
    std::function<void(ViewId, std::uint32_t)> set_view_caret;
    std::function<void(ViewId, std::int64_t)> move_caret_lines;
    std::function<void(ViewId, bool)> move_caret_line_boundary;
    std::function<GuileDeleteOutcome(ViewId, bool, bool)> delete_grapheme;
    std::function<void(ViewId)> newline;
    std::function<std::optional<std::string>(ViewId)> indent;
    std::function<void(ViewId, std::string_view)> type_text;
    std::function<int()> page_rows;
    std::function<GuileInteractionStatus()> interaction_status;
    std::function<std::optional<std::string>()> interaction_provider;
    std::function<std::optional<ProjectId>()> interaction_origin_project;
    std::function<void()> refresh_interaction;
    std::function<std::expected<CommandDispatch, std::string>()> submit_interaction;
    std::function<bool(int)> move_interaction_candidate;
    std::function<bool(int)> move_interaction_history;
    std::function<bool()> cancel_interaction;
    std::function<void()> cancel_pending_input;
    std::function<GuileViewPosition(ViewId)> view_position;
    std::function<GuileLocationNavigation()> location_navigation;
    std::function<std::expected<void, std::string>(std::optional<BufferId>,
                                                   std::optional<std::size_t>)>
        set_location_navigation;
    std::function<std::expected<void, std::string>(WindowId, BufferId, std::uint32_t)>
        position_buffer_view;
    std::function<void(std::string)> set_message;
    std::function<std::expected<void, std::string>(ProjectId)> request_project_index;
    std::function<std::expected<std::string, std::string>(BufferId)> begin_buffer_save;
    std::function<std::expected<bool, std::string>(BufferId)> complete_buffer_save;
    std::function<void(BufferId)> abort_buffer_save;
    std::function<std::vector<BufferId>()> open_buffers;
    std::function<std::expected<BufferId, std::string>(GuileBufferCreation)> create_buffer;
    std::function<bool(BufferId)> buffer_saving;
    std::function<std::expected<void, std::string>(BufferId, BufferId)> release_buffer;
    std::function<void()> request_exit;
    std::function<std::expected<void, std::string>(WindowId, WindowSplitAxis)> split_window;
    std::function<std::expected<void, std::string>(WindowId)> delete_window;
    std::function<std::expected<void, std::string>(WindowId)> delete_other_windows;
    std::function<std::vector<WindowId>()> open_windows;
    std::function<WindowId()> active_window;
    std::function<std::expected<void, std::string>(WindowId)> focus_window;
    std::function<void()> request_redraw;
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
    std::function<std::optional<GuileTextRange>(ViewId)> soft_kill_range;
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
    std::expected<StartupPlan, std::string> startup_plan(const StartupFacts& facts) const;
    std::expected<void, std::string> set_startup_placeholder(std::optional<BufferId> buffer);
    std::expected<bool, std::string> handle_pointer(const CommandContext& context,
                                                    const PointerEvent& event,
                                                    bool pending_key_sequence) const;
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
    std::expected<std::string, std::string> idle_echo_text(const CommandContext& context) const;
    std::expected<ModelineContent, std::string> modeline_content(const CommandContext& context,
                                                                 const ModelineFacts& facts) const;
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
