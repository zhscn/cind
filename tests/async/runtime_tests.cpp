#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "async/runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace cind;

namespace {

class WakeSignal {
public:
    void notify() {
        {
            std::scoped_lock lock(mutex_);
            notified_ = true;
        }
        changed_.notify_one();
    }

    bool wait() {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(2), [this] { return notified_; })) {
            return false;
        }
        notified_ = false;
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool notified_ = false;
};

} // namespace

TEST_CASE("libuv runtime returns worker results to the owning thread") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    const std::thread::id owner = std::this_thread::get_id();
    std::thread::id worker;
    std::thread::id completion_thread;
    int result = 0;

    const AsyncTaskId task = runtime.submit({
        .work = [&](const std::stop_token&) -> AsyncCompletion {
            worker = std::this_thread::get_id();
            return [&] {
                completion_thread = std::this_thread::get_id();
                result = 42;
            };
        },
        .cancelled = {},
        .failed = {},
    });

    CHECK(task.valid());
    CHECK(runtime.has_work());
    REQUIRE(wake.wait());
    CHECK(runtime.drain() == 1);
    CHECK(result == 42);
    CHECK(worker != owner);
    CHECK(completion_thread == owner);
    CHECK_FALSE(runtime.has_work());
}

TEST_CASE("libuv runtime cancellation is cooperative for running work") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    std::latch started(1);
    std::latch release(1);
    std::atomic_bool observed_cancellation = false;
    bool completed = false;
    bool cancelled = false;

    const AsyncTaskId task = runtime.submit({
        .work = [&](const std::stop_token& token) -> AsyncCompletion {
            started.count_down();
            release.wait();
            observed_cancellation.store(token.stop_requested(), std::memory_order_relaxed);
            return [&] { completed = true; };
        },
        .cancelled = [&] { cancelled = true; },
        .failed = {},
    });

    started.wait();
    CHECK(runtime.cancel(task));
    release.count_down();
    REQUIRE(wake.wait());
    CHECK(runtime.drain() == 1);
    CHECK(observed_cancellation.load(std::memory_order_relaxed));
    CHECK(cancelled);
    CHECK_FALSE(completed);
    CHECK_FALSE(runtime.has_work());
}

TEST_CASE("libuv async submission wakeups may coalesce without losing tasks") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    constexpr int task_count = 12;
    int completed = 0;
    for (int index = 0; index < task_count; ++index) {
        (void)runtime.submit({
            .work = [&](const std::stop_token&) -> AsyncCompletion { return [&] { ++completed; }; },
            .cancelled = {},
            .failed = {},
        });
    }

    while (completed != task_count) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK_FALSE(runtime.has_work());
}

TEST_CASE("libuv runtime reports worker failures on the owning thread") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    const std::thread::id owner = std::this_thread::get_id();
    std::thread::id failure_thread;
    std::string message;

    (void)runtime.submit({
        .work = [](const std::stop_token&) -> AsyncCompletion {
            throw std::runtime_error("worker failed");
        },
        .cancelled = {},
        .failed =
            [&](const std::exception_ptr& failure) {
                failure_thread = std::this_thread::get_id();
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& exception) {
                    message = exception.what();
                }
            },
    });

    REQUIRE(wake.wait());
    CHECK(runtime.drain() == 1);
    CHECK(failure_thread == owner);
    CHECK(message == "worker failed");
    CHECK_FALSE(runtime.has_work());
}

TEST_CASE("libuv runtime shutdown requests stop and suppresses delivery") {
    std::latch started(1);
    std::atomic_bool observed_stop = false;
    bool delivered = false;

    {
        AsyncRuntime runtime;
        (void)runtime.submit({
            .work = [&](const std::stop_token& token) -> AsyncCompletion {
                started.count_down();
                while (!token.stop_requested()) {
                    std::this_thread::yield();
                }
                observed_stop.store(true, std::memory_order_relaxed);
                return [&] { delivered = true; };
            },
            .cancelled = [&] { delivered = true; },
            .failed = [&](const std::exception_ptr&) { delivered = true; },
        });
        started.wait();
    }

    CHECK(observed_stop.load(std::memory_order_relaxed));
    CHECK_FALSE(delivered);
}
