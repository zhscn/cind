#pragma once

#include "editor/ids.hpp"
#include "editor/selection.hpp"
#include "editor/window.hpp"

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

struct GuileTextRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

struct GuileHostServices {
    std::function<std::expected<void, std::string>(WindowId, BufferId)> display_buffer;
    std::function<std::expected<void, std::string>(ViewId, std::uint32_t, std::uint32_t)>
        move_caret_to_line;
    std::function<void(std::string)> set_message;
    std::function<std::expected<void, std::string>(ProjectId)> ensure_project_index;
    std::function<std::expected<void, std::string>(WindowId, std::string)> open_file;
    std::function<std::expected<void, std::string>(ProjectId, WindowId, std::string)>
        start_project_search;
    std::function<std::expected<void, std::string>(BufferId, std::string)> set_buffer_resource;
    std::function<void(BufferId)> save_buffer;
    std::function<std::vector<BufferId>()> open_buffers;
    std::function<std::expected<void, std::string>(BufferId, bool)> kill_buffer;
    std::function<void(bool)> request_quit;
    std::function<std::expected<void, std::string>(WindowId, WindowSplitAxis)> split_window;
    std::function<std::expected<void, std::string>(WindowId)> delete_window;
    std::function<void(WindowId)> delete_other_windows;
    std::function<std::expected<void, std::string>(WindowId, int)> select_other_window;
    std::function<void()> request_redraw;
    std::function<std::vector<GuileKeyBindingSummary>()> active_key_bindings;
    std::function<std::vector<KeymapId>(WindowId)> base_keymap_layers;
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
    std::expected<void, std::string> load_extension(const std::string& path);
    GuileRuntimeSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind
