#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>

namespace cind {

struct AsyncTaskId {
    std::uint64_t value = 0;

    bool valid() const { return value != 0; }
    bool operator==(const AsyncTaskId&) const = default;
};

using AsyncCompletion = std::function<void()>;
using AsyncWork = std::function<AsyncCompletion(const std::stop_token&)>;
using AsyncFailure = std::function<void(const std::exception_ptr&)>;

struct AsyncTaskSpec {
    AsyncWork work;
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

    // Runs ready callbacks on the caller. Editor state remains single-writer
    // when frontends call this only from their event-loop thread.
    std::size_t drain();
    bool has_work() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cind
