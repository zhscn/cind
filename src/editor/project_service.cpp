#include "editor/project_service.hpp"

#include "editor/project.hpp"
#include "editor/runtime.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>

namespace cind {

ProjectService::ProjectService(EditorRuntime& runtime, AsyncRuntime& async_runtime,
                               IndexUpdated index_updated)
    : runtime_(&runtime), async_runtime_(&async_runtime), index_updated_(std::move(index_updated)) {
}

ProjectService::State& ProjectService::state(ProjectId project) {
    if (State* existing = find_state(project)) {
        return *existing;
    }
    states_.push_back(std::make_unique<State>(State{.project = project,
                                                    .generation = 0,
                                                    .index_task = {},
                                                    .refresh_pending = false,
                                                    .watches = {}}));
    return *states_.back();
}

ProjectService::State* ProjectService::find_state(ProjectId project) {
    const auto found =
        std::ranges::find_if(states_, [project](const std::unique_ptr<State>& candidate) {
            return candidate->project == project;
        });
    return found == states_.end() ? nullptr : found->get();
}

void ProjectService::request_index(ProjectId project) {
    State& project_state = state(project);
    if (project_state.index_task.valid()) {
        project_state.refresh_pending = true;
        return;
    }
    const Project* definition = runtime_->projects().try_get(project);
    if (definition == nullptr || definition->roots().empty()) {
        return;
    }
    const std::uint64_t generation = ++project_state.generation;
    auto root = std::make_shared<const std::string>(definition->roots().front());
    runtime_->projects().begin_index(project);
    try {
        project_state.index_task = async_runtime_->submit({
            .work = [this, project, generation,
                     root](const std::stop_token& cancellation) -> AsyncCompletion {
                std::expected<ProjectFileIndex, std::error_code> index =
                    scan_project_files(*root, 200'000, cancellation);
                if (!index) {
                    if (index.error() == std::make_error_code(std::errc::operation_canceled)) {
                        throw AsyncTaskCancelled();
                    }
                    throw std::system_error(index.error(),
                                            std::format("cannot index project {}", *root));
                }
                auto result = std::make_shared<ProjectFileIndex>(std::move(*index));
                return [this, project, generation, result] {
                    finish_index(project, generation, std::move(*result));
                };
            },
            .cancelled = [this, project, generation] { cancel_index(project, generation); },
            .failed =
                [this, project, generation](const std::exception_ptr& failure) {
                    fail_index(project, generation, failure);
                },
        });
    } catch (const std::exception& exception) {
        project_state.index_task = {};
        runtime_->projects().fail_index(project, exception.what());
    }
}

void ProjectService::finish_index(ProjectId project, std::uint64_t generation,
                                  ProjectFileIndex index) {
    State* project_state = find_state(project);
    if (project_state == nullptr || project_state->generation != generation ||
        runtime_->projects().try_get(project) == nullptr) {
        return;
    }
    project_state->index_task = {};
    runtime_->projects().replace_index(project, std::move(index.files));
    replace_watches(*project_state, index.directories);
    if (index_updated_) {
        index_updated_(project);
    }
    if (project_state->refresh_pending) {
        project_state->refresh_pending = false;
        request_index(project);
    }
}

void ProjectService::fail_index(ProjectId project, std::uint64_t generation,
                                const std::exception_ptr& failure) {
    State* project_state = find_state(project);
    if (project_state == nullptr || project_state->generation != generation ||
        runtime_->projects().try_get(project) == nullptr) {
        return;
    }
    project_state->index_task = {};
    std::string error = "project indexing failed";
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "project indexing failed: unknown error";
    }
    runtime_->projects().fail_index(project, std::move(error));
    if (project_state->refresh_pending) {
        project_state->refresh_pending = false;
        request_index(project);
    }
}

void ProjectService::cancel_index(ProjectId project, std::uint64_t generation) {
    State* project_state = find_state(project);
    if (project_state == nullptr || project_state->generation != generation ||
        runtime_->projects().try_get(project) == nullptr) {
        return;
    }
    project_state->index_task = {};
    runtime_->projects().cancel_index(project);
    if (project_state->refresh_pending) {
        project_state->refresh_pending = false;
        request_index(project);
    }
}

void ProjectService::replace_watches(State& project_state,
                                     const std::vector<std::string>& directories) {
    constexpr std::size_t maximum_watches = 4096;
    const std::size_t count = std::min(directories.size(), maximum_watches);
    const std::span<const std::string> desired = std::span(directories).first(count);
    std::erase_if(project_state.watches, [&](const State::Watch& watch) {
        if (std::ranges::find(desired, watch.directory) != desired.end()) {
            return false;
        }
        (void)async_runtime_->unwatch(watch.id);
        return true;
    });
    project_state.watches.reserve(count);
    for (const std::string& directory : desired) {
        if (std::ranges::any_of(project_state.watches, [&](const State::Watch& watch) {
                return watch.directory == directory;
            })) {
            continue;
        }
        try {
            const AsyncWatchId watch = async_runtime_->watch_directory({
                .path = directory,
                .started = [this, project = project_state.project] { request_index(project); },
                .changed = [this, project = project_state.project](
                               const AsyncWatchEvent&) { request_index(project); },
                .failed = {},
            });
            project_state.watches.push_back({.directory = directory, .id = watch});
        } catch (const std::exception&) {
            break;
        }
    }
}

} // namespace cind
