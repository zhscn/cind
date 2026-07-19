#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "script/async_host.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace cind;

namespace {

template <typename Predicate> void drain_until(AsyncRuntime& runtime, Predicate&& predicate) {
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
    const std::filesystem::path written_file = directory.path() / "written.txt";
    {
        std::ofstream output(file);
        output << "hello";
    }
    AsyncRuntime runtime;
    AsyncScriptHost host(runtime);
    const std::thread::id owner = std::this_thread::get_id();
    ScriptAsyncResult file_result;
    ScriptAsyncResult directory_result;
    ScriptAsyncResult write_result;
    bool file_completed = false;
    bool directory_completed = false;
    bool write_completed = false;
    std::thread::id callback_thread;

    const auto started_file = host.start(ScriptFileReadRequest{.path = file.string()},
                                         {.completed =
                                              [&](std::uint64_t, ScriptAsyncResult result) {
                                                  callback_thread = std::this_thread::get_id();
                                                  file_result = std::move(result);
                                                  file_completed = true;
                                              },
                                          .cancelled = {},
                                          .failed = {}});
    const auto started_directory = host.start(
        ScriptDirectoryListRequest{.path = directory.path().string(), .maximum_entries = 32},
        {.completed =
             [&](std::uint64_t, ScriptAsyncResult result) {
                 directory_result = std::move(result);
                 directory_completed = true;
             },
         .cancelled = {},
         .failed = {}});
    const auto started_write =
        host.start(ScriptFileWriteRequest{.path = written_file.string(), .contents = "written\n"},
                   {.completed =
                        [&](std::uint64_t, ScriptAsyncResult result) {
                            write_result = std::move(result);
                            write_completed = true;
                        },
                    .cancelled = {},
                    .failed = {}});

    REQUIRE(started_file.has_value());
    REQUIRE(started_directory.has_value());
    REQUIRE(started_write.has_value());
    CHECK(host.tasks().size() == 3);
    drain_until(runtime, [&] { return file_completed && directory_completed && write_completed; });
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
    REQUIRE(std::holds_alternative<ScriptFileWriteResult>(write_result));
    CHECK(std::get<ScriptFileWriteResult>(write_result).path == written_file.string());
    std::ifstream written(written_file);
    CHECK(std::string(std::istreambuf_iterator<char>(written), {}) == "written\n");
    CHECK(callback_thread == owner);
    CHECK(host.tasks().empty());
}

TEST_CASE("script async host discovers language style and project metadata") {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / ".git");
    {
        std::ofstream style(directory.path() / ".clang-format");
        style << "IndentWidth: 7\nTabWidth: 9\n";
    }
    const std::filesystem::path file = directory.path() / "sample.cpp";
    AsyncRuntime runtime;
    AsyncScriptHost host(runtime);
    ScriptClangFormatStyleResult style_result;
    ScriptProjectDiscoveryResult project_result;
    bool style_completed = false;
    bool project_completed = false;

    const auto style = host.start(
        ScriptClangFormatStyleRequest{
            .path = file.string(), .fallback_preset = "Google", .fallback_origin = "test fallback"},
        {.completed =
             [&](std::uint64_t, ScriptAsyncResult result) {
                 style_result = std::get<ScriptClangFormatStyleResult>(std::move(result));
                 style_completed = true;
             },
         .cancelled = {},
         .failed = {}});
    const auto project = host.start(
        ScriptProjectDiscoveryRequest{.path = file.string(),
                                      .providers = {{.name = "test.vcs", .markers = {".git"}}}},
        {.completed =
             [&](std::uint64_t, ScriptAsyncResult result) {
                 project_result = std::get<ScriptProjectDiscoveryResult>(std::move(result));
                 project_completed = true;
             },
         .cancelled = {},
         .failed = {}});

    REQUIRE(style.has_value());
    REQUIRE(project.has_value());
    drain_until(runtime, [&] { return style_completed && project_completed; });
    CHECK(style_result.path == file.string());
    CHECK(style_result.found);
    CHECK(style_result.origin == ".clang-format");
    CHECK(style_result.style.indent_width == 7);
    CHECK(style_result.style.tab_width == 9);
    REQUIRE(project_result.discovery.has_value());
    const ProjectDiscovery discovery = project_result.discovery.value_or(ProjectDiscovery{});
    CHECK(discovery.root == directory.path().string());
    CHECK(discovery.provider == "test.vcs");
    CHECK(discovery.marker == ".git");
}

TEST_CASE("script async host applies the requested style fallback") {
    TemporaryDirectory directory;
    AsyncRuntime runtime;
    AsyncScriptHost host(runtime);
    ScriptClangFormatStyleResult style_result;
    bool completed = false;

    const auto task =
        host.start(ScriptClangFormatStyleRequest{.path = (directory.path() / "sample.cpp").string(),
                                                 .fallback_preset = "Google",
                                                 .fallback_origin = "script fallback"},
                   {.completed =
                        [&](std::uint64_t, ScriptAsyncResult result) {
                            style_result =
                                std::get<ScriptClangFormatStyleResult>(std::move(result));
                            completed = true;
                        },
                    .cancelled = {},
                    .failed = {}});

    REQUIRE(task.has_value());
    drain_until(runtime, [&] { return completed; });
    CHECK_FALSE(style_result.found);
    CHECK(style_result.origin == "script fallback");
    CHECK(style_result.style.indent_width == 2);
}

TEST_CASE("script async host parses ripgrep output into location data") {
    AsyncRuntime runtime;
    AsyncScriptHost host(runtime);
    ScriptRgResultParseResult parse_result;
    bool completed = false;
    const std::string output = std::string("./src/main.cpp\0", 15) + "12:7:needle: value\n";

    const auto parse =
        host.start(ScriptRgResultParseRequest{.project_root = "/work/project", .output = output},
                   {.completed =
                        [&](std::uint64_t, ScriptAsyncResult result) {
                            parse_result = std::get<ScriptRgResultParseResult>(std::move(result));
                            completed = true;
                        },
                    .cancelled = {},
                    .failed = {}});

    REQUIRE(parse.has_value());
    drain_until(runtime, [&] { return completed; });
    CHECK(parse_result.text == "src/main.cpp:12:7: needle: value\n");
    REQUIRE(parse_result.locations.size() == 1);
    CHECK(parse_result.locations[0].resource == "/work/project/src/main.cpp");
    CHECK(parse_result.locations[0].target ==
          EncodedLinePosition{.line = 11, .column = 6, .encoding = PositionEncoding::Bytes});
}

TEST_CASE("script async host unifies process completion and cancellation") {
    AsyncRuntime runtime;
    AsyncScriptHost host(runtime);
    ScriptProcessResult process_result;
    bool process_completed = false;
    bool cancelled = false;

    const auto process =
        host.start(ScriptProcessRequest{.file = "/bin/sh",
                                        .arguments = {"-c", "printf output; exit 3"},
                                        .working_directory = {}},
                   {.completed =
                        [&](std::uint64_t, ScriptAsyncResult result) {
                            process_result = std::get<ScriptProcessResult>(std::move(result));
                            process_completed = true;
                        },
                    .cancelled = {},
                    .failed = {}});
    REQUIRE(process.has_value());
    drain_until(runtime, [&] { return process_completed; });
    CHECK(process_result.exit_status == 3);
    CHECK(process_result.standard_output == "output");

    const auto sleeping = host.start(ScriptProcessRequest{.file = "/bin/sh",
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
