#pragma once

#include "async/runtime.hpp"
#include "editor/ids.hpp"
#include "project/project_files.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cind {

class EditorRuntime;

// Owns asynchronous file-index refreshes and native directory watches.
// ProjectRegistry remains the main-thread domain store; Scheme policy creates
// and attaches projects, requests initial indexing and reacts to update events.
class ProjectService {
public:
    using IndexUpdated = std::function<void(ProjectId)>;

    ProjectService(EditorRuntime& runtime, AsyncRuntime& async_runtime,
                   IndexUpdated index_updated = {});

    void request_index(ProjectId project);

private:
    struct State {
        struct Watch {
            std::string directory;
            AsyncWatchId id;
        };

        ProjectId project;
        std::uint64_t generation = 0;
        AsyncTaskId index_task;
        bool refresh_pending = false;
        std::vector<Watch> watches;
    };

    State& state(ProjectId project);
    State* find_state(ProjectId project);
    void finish_index(ProjectId project, std::uint64_t generation, ProjectFileIndex index);
    void fail_index(ProjectId project, std::uint64_t generation, const std::exception_ptr& failure);
    void cancel_index(ProjectId project, std::uint64_t generation);
    void replace_watches(State& state, const std::vector<std::string>& directories);

    EditorRuntime* runtime_;
    AsyncRuntime* async_runtime_;
    IndexUpdated index_updated_;
    std::vector<std::unique_ptr<State>> states_;
};

} // namespace cind
