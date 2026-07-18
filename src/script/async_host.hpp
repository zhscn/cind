#pragma once

#include "async/runtime.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "project/project_files.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cind {

enum class ScriptAsyncTaskKind : std::uint8_t {
    FileRead,
    FileWrite,
    DirectoryList,
    ClangFormatStyle,
    ProjectDiscovery,
    Process,
};

struct ScriptFileReadRequest {
    std::string path;
};

struct ScriptFileWriteRequest {
    std::string path;
    std::string contents;
};

struct ScriptDirectoryListRequest {
    std::string path;
    std::size_t maximum_entries = 4096;
};

struct ScriptClangFormatStyleRequest {
    std::string path;
};

struct ScriptProjectDiscoveryRequest {
    std::string path;
    std::vector<ProjectDiscoveryProvider> providers;
};

struct ScriptProcessRequest {
    std::string file;
    std::vector<std::string> arguments;
    std::string working_directory;
};

using ScriptAsyncRequest = std::variant<ScriptFileReadRequest, ScriptFileWriteRequest,
                                        ScriptDirectoryListRequest, ScriptClangFormatStyleRequest,
                                        ScriptProjectDiscoveryRequest, ScriptProcessRequest>;

struct ScriptFileReadResult {
    std::string path;
    bool exists = false;
    std::string contents;
};

struct ScriptFileWriteResult {
    std::string path;
};

struct ScriptDirectoryEntry {
    std::string path;
    std::string name;
    bool directory = false;
};

struct ScriptDirectoryListResult {
    std::string path;
    std::vector<ScriptDirectoryEntry> entries;
};

struct ScriptClangFormatStyleResult {
    std::string path;
    bool found = false;
    CppIndentStyle style;
    std::string origin;
};

struct ScriptProjectDiscoveryResult {
    std::string path;
    std::optional<ProjectDiscovery> discovery;
};

struct ScriptProcessResult {
    std::int64_t exit_status = 0;
    int term_signal = 0;
    std::string standard_output;
    std::string standard_error;
};

using ScriptAsyncResult =
    std::variant<ScriptFileReadResult, ScriptFileWriteResult, ScriptDirectoryListResult,
                 ScriptClangFormatStyleResult, ScriptProjectDiscoveryResult, ScriptProcessResult>;

struct ScriptAsyncCallbacks {
    std::function<void(std::uint64_t, ScriptAsyncResult)> completed;
    std::function<void(std::uint64_t)> cancelled;
    std::function<void(std::uint64_t, std::string)> failed;
};

struct ScriptAsyncTaskSummary {
    std::uint64_t id = 0;
    ScriptAsyncTaskKind kind = ScriptAsyncTaskKind::FileRead;
};

// Adapts the native async runtime to a stable task protocol for embedded
// languages. Worker callbacks operate only on immutable native values;
// terminal callbacks are delivered by AsyncRuntime::drain() on its owner.
class AsyncScriptHost {
public:
    explicit AsyncScriptHost(AsyncRuntime& runtime);
    ~AsyncScriptHost();

    AsyncScriptHost(const AsyncScriptHost&) = delete;
    AsyncScriptHost& operator=(const AsyncScriptHost&) = delete;

    std::expected<std::uint64_t, std::string> start(ScriptAsyncRequest request,
                                                    ScriptAsyncCallbacks callbacks);
    bool cancel(std::uint64_t task);
    std::vector<ScriptAsyncTaskSummary> tasks() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace cind
