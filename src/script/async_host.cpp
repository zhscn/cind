#include "script/async_host.hpp"

#include "project/search_results.hpp"

#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "formatting/clang_format_style.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace cind {

namespace {

using NativeTask = std::variant<AsyncTaskId, AsyncProcessId>;

struct TaskRecord {
    ScriptAsyncTaskKind kind = ScriptAsyncTaskKind::FileRead;
    NativeTask native;
};

std::string exception_message(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return "unknown asynchronous failure";
    }
}

void require_owner(const std::thread::id& owner) {
    if (std::this_thread::get_id() != owner) {
        throw std::logic_error("script async host used outside its owner thread");
    }
}

} // namespace

struct AsyncScriptHost::State {
    AsyncRuntime* runtime = nullptr;
    std::thread::id owner;
    bool active = true;
    std::uint64_t next_id = 1;
    std::unordered_map<std::uint64_t, TaskRecord> tasks;
};

namespace {

template <typename State, typename Callback>
void finish_task(const std::weak_ptr<State>& weak_state, std::uint64_t id, Callback&& callback) {
    const std::shared_ptr<State> state = weak_state.lock();
    if (!state || !state->active) {
        return;
    }
    require_owner(state->owner);
    if (state->tasks.erase(id) == 0) {
        return;
    }
    std::forward<Callback>(callback)();
}

void throw_file_error(const std::error_code& error) {
    if (error == std::errc::operation_canceled) {
        throw AsyncTaskCancelled{};
    }
    throw std::system_error(error);
}

} // namespace

AsyncScriptHost::AsyncScriptHost(AsyncRuntime& runtime) : state_(std::make_shared<State>()) {
    state_->runtime = &runtime;
    state_->owner = std::this_thread::get_id();
}

AsyncScriptHost::~AsyncScriptHost() {
    if (!state_) {
        return;
    }
    if (std::this_thread::get_id() != state_->owner) {
        std::terminate();
    }
    state_->active = false;
    for (const auto& [id, task] : state_->tasks) {
        (void)id;
        if (const auto* native = std::get_if<AsyncTaskId>(&task.native)) {
            (void)state_->runtime->cancel(*native);
        } else if (const auto* process = std::get_if<AsyncProcessId>(&task.native)) {
            (void)state_->runtime->terminate(*process);
        }
    }
    state_->tasks.clear();
    state_->runtime = nullptr;
}

std::expected<std::uint64_t, std::string> AsyncScriptHost::start(ScriptAsyncRequest request,
                                                                 ScriptAsyncCallbacks callbacks) {
    require_owner(state_->owner);
    if (!state_->active || state_->runtime == nullptr) {
        return std::unexpected("script async host is unavailable");
    }
    if (!callbacks.completed) {
        return std::unexpected("script async task requires a completion callback");
    }
    if (state_->next_id == 0) {
        return std::unexpected("script async task ID space is exhausted");
    }

    const std::uint64_t id = state_->next_id++;
    const std::weak_ptr<State> weak_state = state_;
    try {
        TaskRecord record = std::visit(
            [&](auto&& operation) -> TaskRecord {
                using Request = std::remove_cvref_t<decltype(operation)>;
                if constexpr (std::is_same_v<Request, ScriptFileReadRequest>) {
                    if (operation.path.empty()) {
                        throw std::invalid_argument("file-read path is empty");
                    }
                    const std::string path = operation.path;
                    // Throwing reports worker failures through AsyncRuntime::failed.
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    AsyncWork work = [path, weak_state, id, completed = callbacks.completed](
                                         const std::stop_token& cancellation) mutable {
                        std::expected<FileReadResult, std::error_code> read =
                            read_file_contents(std::filesystem::path(path), cancellation);
                        if (!read) {
                            throw_file_error(read.error());
                        }
                        ScriptAsyncResult result =
                            ScriptFileReadResult{.path = path,
                                                 .exists = read->exists,
                                                 .contents = std::move(read->contents)};
                        return [weak_state, id, completed = std::move(completed),
                                result = std::move(result)]() mutable {
                            finish_task(weak_state, id,
                                        [id, completed = std::move(completed),
                                         result = std::move(result)]() mutable {
                                            completed(id, std::move(result));
                                        });
                        };
                    };
                    const AsyncTaskId native = state_->runtime->submit(
                        {.work = std::move(work),
                         .cancelled =
                             [weak_state, id, cancelled = callbacks.cancelled]() mutable {
                                 finish_task(weak_state, id,
                                             [id, cancelled = std::move(cancelled)] {
                                                 if (cancelled) {
                                                     cancelled(id);
                                                 }
                                             });
                             },
                         .failed =
                             [weak_state, id, failed = callbacks.failed](
                                 const std::exception_ptr& failure) mutable {
                                 const std::string message = exception_message(failure);
                                 finish_task(weak_state, id,
                                             [id, failed = std::move(failed), message] {
                                                 if (failed) {
                                                     failed(id, message);
                                                 }
                                             });
                             }});
                    return {.kind = ScriptAsyncTaskKind::FileRead, .native = native};
                } else if constexpr (std::is_same_v<Request, ScriptFileWriteRequest>) {
                    if (operation.path.empty()) {
                        throw std::invalid_argument("file-write path is empty");
                    }
                    const std::string path = operation.path;
                    Text content(operation.contents);
                    // Throwing reports worker failures through AsyncRuntime::failed.
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    AsyncWork work = [path, content = std::move(content), weak_state, id,
                                      completed = callbacks.completed](
                                         const std::stop_token& cancellation) mutable {
                        const std::error_code error =
                            save_file_atomically(path, content, cancellation);
                        if (error) {
                            throw_file_error(error);
                        }
                        ScriptAsyncResult result = ScriptFileWriteResult{.path = path};
                        return [weak_state, id, completed = std::move(completed),
                                result = std::move(result)]() mutable {
                            finish_task(weak_state, id,
                                        [id, completed = std::move(completed),
                                         result = std::move(result)]() mutable {
                                            completed(id, std::move(result));
                                        });
                        };
                    };
                    const AsyncTaskId native = state_->runtime->submit(
                        {.work = std::move(work),
                         .cancelled =
                             [weak_state, id, cancelled = callbacks.cancelled]() mutable {
                                 finish_task(weak_state, id,
                                             [id, cancelled = std::move(cancelled)] {
                                                 if (cancelled) {
                                                     cancelled(id);
                                                 }
                                             });
                             },
                         .failed =
                             [weak_state, id, failed = callbacks.failed](
                                 const std::exception_ptr& failure) mutable {
                                 const std::string message = exception_message(failure);
                                 finish_task(weak_state, id,
                                             [id, failed = std::move(failed), message] {
                                                 if (failed) {
                                                     failed(id, message);
                                                 }
                                             });
                             }});
                    return {.kind = ScriptAsyncTaskKind::FileWrite, .native = native};
                } else if constexpr (std::is_same_v<Request, ScriptDirectoryListRequest>) {
                    if (operation.path.empty()) {
                        throw std::invalid_argument("directory-list path is empty");
                    }
                    const std::string path = operation.path;
                    const std::size_t maximum_entries = operation.maximum_entries;
                    // Throwing reports worker failures through AsyncRuntime::failed.
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    AsyncWork work = [path, maximum_entries, weak_state, id,
                                      completed = callbacks.completed](
                                         const std::stop_token& cancellation) mutable {
                        std::expected<DirectoryListing, std::error_code> listing = list_directory(
                            std::filesystem::path(path), maximum_entries, cancellation);
                        if (!listing) {
                            throw_file_error(listing.error());
                        }
                        ScriptDirectoryListResult directory{.path = listing->directory.string(),
                                                            .entries = {}};
                        directory.entries.reserve(listing->entries.size());
                        for (DirectoryEntry& entry : listing->entries) {
                            directory.entries.push_back({.path = entry.path.string(),
                                                         .name = std::move(entry.name),
                                                         .directory = entry.directory});
                        }
                        ScriptAsyncResult result = std::move(directory);
                        return [weak_state, id, completed = std::move(completed),
                                result = std::move(result)]() mutable {
                            finish_task(weak_state, id,
                                        [id, completed = std::move(completed),
                                         result = std::move(result)]() mutable {
                                            completed(id, std::move(result));
                                        });
                        };
                    };
                    const AsyncTaskId native = state_->runtime->submit(
                        {.work = std::move(work),
                         .cancelled =
                             [weak_state, id, cancelled = callbacks.cancelled]() mutable {
                                 finish_task(weak_state, id,
                                             [id, cancelled = std::move(cancelled)] {
                                                 if (cancelled) {
                                                     cancelled(id);
                                                 }
                                             });
                             },
                         .failed =
                             [weak_state, id, failed = callbacks.failed](
                                 const std::exception_ptr& failure) mutable {
                                 const std::string message = exception_message(failure);
                                 finish_task(weak_state, id,
                                             [id, failed = std::move(failed), message] {
                                                 if (failed) {
                                                     failed(id, message);
                                                 }
                                             });
                             }});
                    return {.kind = ScriptAsyncTaskKind::DirectoryList, .native = native};
                } else if constexpr (std::is_same_v<Request, ScriptClangFormatStyleRequest>) {
                    if (operation.path.empty()) {
                        throw std::invalid_argument("clang-format-style path is empty");
                    }
                    const std::string path = operation.path;
                    CppIndentStyle fallback;
                    if (!apply_clang_format_preset(operation.fallback_preset, fallback)) {
                        throw std::invalid_argument("unknown clang-format fallback preset: " +
                                                    operation.fallback_preset);
                    }
                    const std::string fallback_origin = operation.fallback_origin;
                    // Throwing reports worker failures through AsyncRuntime::failed.
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    AsyncWork work = [path, fallback, fallback_origin, weak_state, id,
                                      completed = callbacks.completed](
                                         const std::stop_token& cancellation) mutable {
                        if (cancellation.stop_requested()) {
                            throw AsyncTaskCancelled{};
                        }
                        ScriptClangFormatStyleResult style{
                            .path = path,
                            .found = false,
                            .style = fallback,
                            .origin = fallback_origin,
                        };
                        if (std::optional<LoadedStyle> loaded = load_clang_format_style(
                                std::filesystem::path(path).parent_path())) {
                            style.found = true;
                            style.style = loaded->style;
                            style.origin = loaded->config_path.filename().string();
                        }
                        if (cancellation.stop_requested()) {
                            throw AsyncTaskCancelled{};
                        }
                        ScriptAsyncResult result = std::move(style);
                        return [weak_state, id, completed = std::move(completed),
                                result = std::move(result)]() mutable {
                            finish_task(weak_state, id,
                                        [id, completed = std::move(completed),
                                         result = std::move(result)]() mutable {
                                            completed(id, std::move(result));
                                        });
                        };
                    };
                    const AsyncTaskId native = state_->runtime->submit(
                        {.work = std::move(work),
                         .cancelled =
                             [weak_state, id, cancelled = callbacks.cancelled]() mutable {
                                 finish_task(weak_state, id,
                                             [id, cancelled = std::move(cancelled)] {
                                                 if (cancelled) {
                                                     cancelled(id);
                                                 }
                                             });
                             },
                         .failed =
                             [weak_state, id, failed = callbacks.failed](
                                 const std::exception_ptr& failure) mutable {
                                 const std::string message = exception_message(failure);
                                 finish_task(weak_state, id,
                                             [id, failed = std::move(failed), message] {
                                                 if (failed) {
                                                     failed(id, message);
                                                 }
                                             });
                             }});
                    return {.kind = ScriptAsyncTaskKind::ClangFormatStyle, .native = native};
                } else if constexpr (std::is_same_v<Request, ScriptProjectDiscoveryRequest>) {
                    if (operation.path.empty()) {
                        throw std::invalid_argument("project-discovery path is empty");
                    }
                    const std::string path = operation.path;
                    std::vector<ProjectDiscoveryProvider> providers =
                        std::move(operation.providers);
                    // Throwing reports worker failures through AsyncRuntime::failed.
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    AsyncWork work = [path, providers = std::move(providers), weak_state, id,
                                      completed = callbacks.completed](
                                         const std::stop_token& cancellation) mutable {
                        std::expected<std::optional<ProjectDiscovery>, std::error_code> discovery =
                            discover_project(path, providers, cancellation);
                        if (!discovery) {
                            throw_file_error(discovery.error());
                        }
                        ScriptAsyncResult result = ScriptProjectDiscoveryResult{
                            .path = path,
                            .discovery = std::move(*discovery),
                        };
                        return [weak_state, id, completed = std::move(completed),
                                result = std::move(result)]() mutable {
                            finish_task(weak_state, id,
                                        [id, completed = std::move(completed),
                                         result = std::move(result)]() mutable {
                                            completed(id, std::move(result));
                                        });
                        };
                    };
                    const AsyncTaskId native = state_->runtime->submit(
                        {.work = std::move(work),
                         .cancelled =
                             [weak_state, id, cancelled = callbacks.cancelled]() mutable {
                                 finish_task(weak_state, id,
                                             [id, cancelled = std::move(cancelled)] {
                                                 if (cancelled) {
                                                     cancelled(id);
                                                 }
                                             });
                             },
                         .failed =
                             [weak_state, id, failed = callbacks.failed](
                                 const std::exception_ptr& failure) mutable {
                                 const std::string message = exception_message(failure);
                                 finish_task(weak_state, id,
                                             [id, failed = std::move(failed), message] {
                                                 if (failed) {
                                                     failed(id, message);
                                                 }
                                             });
                             }});
                    return {.kind = ScriptAsyncTaskKind::ProjectDiscovery, .native = native};
                } else if constexpr (std::is_same_v<Request, ScriptRgResultParseRequest>) {
                    if (operation.project_root.empty()) {
                        throw std::invalid_argument("rg-result-parse project root is empty");
                    }
                    std::string project_root = std::move(operation.project_root);
                    std::string output = std::move(operation.output);
                    // Throwing reports worker failures through AsyncRuntime::failed.
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    AsyncWork work = [project_root = std::move(project_root),
                                      output = std::move(output), weak_state, id,
                                      completed = callbacks.completed](
                                         const std::stop_token& cancellation) mutable {
                        if (cancellation.stop_requested()) {
                            throw AsyncTaskCancelled{};
                        }
                        std::expected<LocationListDocument, std::string> parsed =
                            parse_rg_search_results(
                                {.project_root = project_root, .output = output});
                        if (!parsed) {
                            throw std::runtime_error(parsed.error());
                        }
                        if (cancellation.stop_requested()) {
                            throw AsyncTaskCancelled{};
                        }
                        ScriptAsyncResult result = ScriptRgResultParseResult{
                            .text = std::move(parsed->text),
                            .locations = std::move(parsed->locations),
                        };
                        return [weak_state, id, completed = std::move(completed),
                                result = std::move(result)]() mutable {
                            finish_task(weak_state, id,
                                        [id, completed = std::move(completed),
                                         result = std::move(result)]() mutable {
                                            completed(id, std::move(result));
                                        });
                        };
                    };
                    const AsyncTaskId native = state_->runtime->submit(
                        {.work = std::move(work),
                         .cancelled =
                             [weak_state, id, cancelled = callbacks.cancelled]() mutable {
                                 finish_task(weak_state, id,
                                             [id, cancelled = std::move(cancelled)] {
                                                 if (cancelled) {
                                                     cancelled(id);
                                                 }
                                             });
                             },
                         .failed =
                             [weak_state, id, failed = callbacks.failed](
                                 const std::exception_ptr& failure) mutable {
                                 const std::string message = exception_message(failure);
                                 finish_task(weak_state, id,
                                             [id, failed = std::move(failed), message] {
                                                 if (failed) {
                                                     failed(id, message);
                                                 }
                                             });
                             }});
                    return {.kind = ScriptAsyncTaskKind::RgResultParse, .native = native};
                } else {
                    if (operation.file.empty()) {
                        throw std::invalid_argument("process executable is empty");
                    }
                    const AsyncProcessId native = state_->runtime->spawn(
                        {.file = std::move(operation.file),
                         .arguments = std::move(operation.arguments),
                         .working_directory = std::move(operation.working_directory),
                         .completed =
                             [weak_state, id, completed = callbacks.completed](
                                 AsyncProcessResult native_result) mutable {
                                 ScriptAsyncResult result = ScriptProcessResult{
                                     .exit_status = native_result.exit_status,
                                     .term_signal = native_result.term_signal,
                                     .standard_output = std::move(native_result.standard_output),
                                     .standard_error = std::move(native_result.standard_error)};
                                 finish_task(weak_state, id,
                                             [id, completed = std::move(completed),
                                              result = std::move(result)]() mutable {
                                                 completed(id, std::move(result));
                                             });
                             },
                         .cancelled =
                             [weak_state, id, cancelled = callbacks.cancelled]() mutable {
                                 finish_task(weak_state, id,
                                             [id, cancelled = std::move(cancelled)] {
                                                 if (cancelled) {
                                                     cancelled(id);
                                                 }
                                             });
                             },
                         .failed =
                             [weak_state, id, failed = callbacks.failed](
                                 const std::exception_ptr& failure) mutable {
                                 const std::string message = exception_message(failure);
                                 finish_task(weak_state, id,
                                             [id, failed = std::move(failed), message] {
                                                 if (failed) {
                                                     failed(id, message);
                                                 }
                                             });
                             }});
                    return {.kind = ScriptAsyncTaskKind::Process, .native = native};
                }
            },
            std::move(request));
        state_->tasks.emplace(id, record);
        return id;
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

bool AsyncScriptHost::cancel(std::uint64_t task) {
    require_owner(state_->owner);
    if (!state_->active || state_->runtime == nullptr) {
        return false;
    }
    const auto found = state_->tasks.find(task);
    if (found == state_->tasks.end()) {
        return false;
    }
    return std::visit(
        [runtime = state_->runtime](const auto native) {
            using Native = std::remove_cvref_t<decltype(native)>;
            if constexpr (std::is_same_v<Native, AsyncTaskId>) {
                return runtime->cancel(native);
            } else {
                return runtime->terminate(native);
            }
        },
        found->second.native);
}

std::vector<ScriptAsyncTaskSummary> AsyncScriptHost::tasks() const {
    require_owner(state_->owner);
    std::vector<ScriptAsyncTaskSummary> result;
    result.reserve(state_->tasks.size());
    for (const auto& [id, task] : state_->tasks) {
        result.push_back({.id = id, .kind = task.kind});
    }
    std::ranges::sort(result, {}, &ScriptAsyncTaskSummary::id);
    return result;
}

} // namespace cind
