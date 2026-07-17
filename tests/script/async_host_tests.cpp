#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "script/async_host.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>
#include <thread>

using namespace cind;

namespace {

template <typename Predicate>
void drain_until(AsyncRuntime& runtime, Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        (void)runtime.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)runtime.drain();
    REQUIRE(predicate());
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                std::format("cind-script-async-{}",
                            std::chrono::steady_clock::now().time_since_epoch().count())) {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("script async host delivers file and directory results on its owner") {
    TemporaryDirectory directory;
    const std::filesystem::path file = directory.path() / "value.txt";
    {
        std::ofstream output(file);
        output << "hello";
    }
    AsyncRuntime runtime;
    AsyncScriptHost host(runtime);
    const std::thread::id owner = std::this_thread::get_id();
    ScriptAsyncResult file_result;
    ScriptAsyncResult directory_result;
    bool file_completed = false;
    bool directory_completed = false;
    std::thread::id callback_thread;

    const auto started_file = host.start(
        ScriptFileReadRequest{.path = file.string()},
         {.completed = [&](std::uint64_t, ScriptAsyncResult result) {
             callback_thread = std::this_thread::get_id();
             file_result = std::move(result);
             file_completed = true;
         },
         .cancelled = {},
         .failed = {}});
    const auto started_directory = host.start(
        ScriptDirectoryListRequest{.path = directory.path().string(), .maximum_entries = 32},
         {.completed = [&](std::uint64_t, ScriptAsyncResult result) {
             directory_result = std::move(result);
             directory_completed = true;
         },
         .cancelled = {},
         .failed = {}});

    REQUIRE(started_file.has_value());
    REQUIRE(started_directory.has_value());
    CHECK(host.tasks().size() == 2);
    drain_until(runtime, [&] { return file_completed && directory_completed; });
    REQUIRE(std::holds_alternative<ScriptFileReadResult>(file_result));
    const ScriptFileReadResult& read = std::get<ScriptFileReadResult>(file_result);
    CHECK(read.path == file.string());
    CHECK(read.exists);
    CHECK(read.contents == "hello");
    REQUIRE(std::holds_alternative<ScriptDirectoryListResult>(directory_result));
    const ScriptDirectoryListResult& listing =
        std::get<ScriptDirectoryListResult>(directory_result);
    CHECK(std::ranges::any_of(listing.entries, [](const ScriptDirectoryEntry& entry) {
        return entry.name == "value.txt" && !entry.directory;
    }));
    CHECK(callback_thread == owner);
    CHECK(host.tasks().empty());
}

TEST_CASE("script async host unifies process completion and cancellation") {
    AsyncRuntime runtime;
    AsyncScriptHost host(runtime);
    ScriptProcessResult process_result;
    bool process_completed = false;
    bool cancelled = false;

    const auto process = host.start(
        ScriptProcessRequest{.file = "/bin/sh",
                             .arguments = {"-c", "printf output; exit 3"},
                             .working_directory = {}},
         {.completed = [&](std::uint64_t, ScriptAsyncResult result) {
             process_result = std::get<ScriptProcessResult>(std::move(result));
             process_completed = true;
         },
         .cancelled = {},
         .failed = {}});
    REQUIRE(process.has_value());
    drain_until(runtime, [&] { return process_completed; });
    CHECK(process_result.exit_status == 3);
    CHECK(process_result.standard_output == "output");

    const auto sleeping = host.start(
        ScriptProcessRequest{.file = "/bin/sh",
                             .arguments = {"-c", "sleep 30"},
                             .working_directory = {}},
        {.completed = [](std::uint64_t, const ScriptAsyncResult&) {},
         .cancelled = [&](std::uint64_t) { cancelled = true; },
         .failed = {}});
    REQUIRE(sleeping.has_value());
    CHECK(host.cancel(*sleeping));
    drain_until(runtime, [&] { return cancelled; });
    CHECK(host.tasks().empty());
}
