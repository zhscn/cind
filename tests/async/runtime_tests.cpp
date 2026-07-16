#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "async/runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
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
            if (token.stop_requested()) {
                throw AsyncTaskCancelled();
            }
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

TEST_CASE("a stop request does not discard work committed by the worker") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    std::latch started(1);
    std::latch release(1);
    bool completed = false;
    bool cancelled = false;

    const AsyncTaskId task = runtime.submit({
        .work = [&](const std::stop_token&) -> AsyncCompletion {
            started.count_down();
            release.wait();
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
    CHECK(completed);
    CHECK_FALSE(cancelled);
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

TEST_CASE("directory watches deliver filesystem changes on the owning thread") {
    namespace fs = std::filesystem;
    const fs::path directory =
        fs::temp_directory_path() /
        std::format("cind-watch-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directory(directory);

    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    const std::thread::id owner = std::this_thread::get_id();
    bool started = false;
    bool changed = false;
    std::thread::id callback_thread;
    const AsyncWatchId watch = runtime.watch_directory({
        .path = directory.string(),
        .started = [&] { started = true; },
        .changed =
            [&](const AsyncWatchEvent& event) {
                callback_thread = std::this_thread::get_id();
                changed = event.path.ends_with("created.txt");
            },
        .failed = {},
    });

    REQUIRE(wake.wait());
    (void)runtime.drain();
    REQUIRE(started);
    {
        std::ofstream file(directory / "created.txt");
        file << "content";
    }
    while (!changed) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(callback_thread == owner);
    CHECK(runtime.unwatch(watch));
    CHECK_FALSE(runtime.unwatch(watch));
    fs::remove_all(directory);
}

TEST_CASE("process service captures output and exit status on the owning thread") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    const std::thread::id owner = std::this_thread::get_id();
    std::thread::id callback_thread;
    std::optional<AsyncProcessResult> result;

    const AsyncProcessId process = runtime.spawn({
        .file = "/bin/sh",
        .arguments = {"-c", "printf stdout; printf stderr >&2; exit 7"},
        .working_directory = {},
        .completed =
            [&](AsyncProcessResult completed) {
                callback_thread = std::this_thread::get_id();
                result = std::move(completed);
            },
        .cancelled = {},
        .failed = {},
    });

    REQUIRE(process.valid());
    REQUIRE(wake.wait());
    CHECK(runtime.drain() == 1);
    REQUIRE(result.has_value());
    CHECK(result->process == process);
    CHECK(result->exit_status == 7);
    CHECK(result->term_signal == 0);
    CHECK(result->standard_output == "stdout");
    CHECK(result->standard_error == "stderr");
    CHECK(callback_thread == owner);
    CHECK_FALSE(runtime.has_work());
}

TEST_CASE("process service supports cancellation before a process starts") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    bool completed = false;
    bool cancelled = false;
    const AsyncProcessId process = runtime.spawn({
        .file = "/bin/sh",
        .arguments = {"-c", "sleep 30"},
        .working_directory = {},
        .completed = [&](AsyncProcessResult) { completed = true; },
        .cancelled = [&] { cancelled = true; },
        .failed = {},
    });
    CHECK(runtime.terminate(process));
    CHECK_FALSE(runtime.terminate(process));
    REQUIRE(wake.wait());
    CHECK(runtime.drain() == 1);
    CHECK(cancelled);
    CHECK_FALSE(completed);
    CHECK_FALSE(runtime.has_work());
}

TEST_CASE("process service reports spawn failures") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    std::string failure_message;
    (void)runtime.spawn({
        .file = "/cind/does/not/exist",
        .arguments = {},
        .working_directory = {},
        .completed = [](AsyncProcessResult) {},
        .cancelled = {},
        .failed =
            [&](const std::exception_ptr& failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& exception) {
                    failure_message = exception.what();
                }
            },
    });
    REQUIRE(wake.wait());
    CHECK(runtime.drain() == 1);
    CHECK(failure_message.find("cannot start process") != std::string::npos);
    CHECK_FALSE(runtime.has_work());
}

TEST_CASE("runtime shutdown terminates a child process and suppresses delivery") {
    namespace fs = std::filesystem;
    const fs::path marker =
        fs::temp_directory_path() /
        std::format("cind-process-started-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    bool delivered = false;
    {
        AsyncRuntime runtime;
        (void)runtime.spawn({
            .file = "/bin/sh",
            .arguments = {"-c", std::format("touch {}; sleep 30", marker.string())},
            .working_directory = {},
            .completed = [&](AsyncProcessResult) { delivered = true; },
            .cancelled = [&] { delivered = true; },
            .failed = [&](const std::exception_ptr&) { delivered = true; },
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!fs::exists(marker) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        REQUIRE(fs::exists(marker));
    }
    CHECK_FALSE(delivered);
    fs::remove(marker);
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
                throw AsyncTaskCancelled();
            },
            .cancelled = [&] { delivered = true; },
            .failed = [&](const std::exception_ptr&) { delivered = true; },
        });
        started.wait();
    }

    CHECK(observed_stop.load(std::memory_order_relaxed));
    CHECK_FALSE(delivered);
}
