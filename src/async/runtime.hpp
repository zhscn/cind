#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace cind {

struct AsyncTaskId {
    std::uint64_t value = 0;

    bool valid() const { return value != 0; }
    bool operator==(const AsyncTaskId&) const = default;
};

struct AsyncWatchId {
    std::uint64_t value = 0;

    bool valid() const { return value != 0; }
    bool operator==(const AsyncWatchId&) const = default;
};

struct AsyncProcessId {
    std::uint64_t value = 0;

    bool valid() const { return value != 0; }
    bool operator==(const AsyncProcessId&) const = default;
};

struct AsyncWatchEvent {
    AsyncWatchId watch;
    std::string directory;
    std::string path;
    bool renamed = false;
    bool changed = false;
};

struct AsyncProcessResult {
    AsyncProcessId process;
    std::int64_t exit_status = 0;
    int term_signal = 0;
    std::string standard_output;
    std::string standard_error;
};

// Worker code throws this only after observing a stop request at a point where
// abandoning the operation preserves its external invariants. A stop request
// alone does not imply that already-running work was cancelled.
class AsyncTaskCancelled final : public std::exception {
public:
    const char* what() const noexcept override { return "async task cancelled"; }
};

using AsyncCompletion = std::function<void()>;
using AsyncWork = std::function<AsyncCompletion(const std::stop_token&)>;
using AsyncFailure = std::function<void(const std::exception_ptr&)>;

struct AsyncTaskSpec {
    AsyncWork work;
    AsyncCompletion cancelled;
    AsyncFailure failed;
};

struct AsyncDirectoryWatchSpec {
    std::string path;
    AsyncCompletion started;
    std::function<void(const AsyncWatchEvent&)> changed;
    AsyncFailure failed;
};

struct AsyncProcessSpec {
    std::string file;
    std::vector<std::string> arguments;
    std::string working_directory;
    std::function<void(AsyncProcessId)> started;
    std::function<void(AsyncProcessId, std::string)> standard_output;
    std::function<void(AsyncProcessId, std::string)> standard_error;
    std::function<void(AsyncProcessResult)> completed;
    AsyncCompletion cancelled;
    AsyncFailure failed;
};

// Runs blocking work on libuv's worker pool and transfers all state-changing
// completion callbacks to the editor's owning thread. The libuv loop lives on
// a dedicated thread; frontend event loops receive only a thread-safe wakeup
// notification and call drain() on their own thread.
class AsyncRuntime {
public:
    // The frontend wakeup may run on the libuv loop thread. It must be
    // thread-safe and non-throwing, and it must not access editor state.
    using Wakeup = std::function<void()>;

    explicit AsyncRuntime(Wakeup wakeup = {});
    ~AsyncRuntime();
    AsyncRuntime(const AsyncRuntime&) = delete;
    AsyncRuntime& operator=(const AsyncRuntime&) = delete;
    AsyncRuntime(AsyncRuntime&&) = delete;
    AsyncRuntime& operator=(AsyncRuntime&&) = delete;

    AsyncTaskId submit(AsyncTaskSpec spec);
    bool cancel(AsyncTaskId task) noexcept;
    AsyncWatchId watch_directory(AsyncDirectoryWatchSpec spec);
    bool unwatch(AsyncWatchId watch) noexcept;
    AsyncProcessId spawn(AsyncProcessSpec spec);
    bool write(AsyncProcessId process, std::string data);
    bool close_input(AsyncProcessId process) noexcept;
    bool terminate(AsyncProcessId process) noexcept;

    // Runs ready callbacks on the caller. Editor state remains single-writer
    // when frontends call this only from their event-loop thread.
    std::size_t drain();
    bool has_work() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind
