#pragma once

#include "editor/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cind {

class EditorRuntime;

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
};

struct GuileRuntimeSnapshot {
    std::string engine;
    std::string version;
    std::vector<std::string> modules;
    std::uint64_t command_revision = 0;
    std::size_t scripted_commands = 0;
    std::uint64_t binding_revision = 0;
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
    std::expected<std::size_t, std::string> install_default_keymaps();
    GuileRuntimeSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind
