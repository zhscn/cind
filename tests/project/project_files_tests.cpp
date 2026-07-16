#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "project/project_files.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>

using namespace cind;

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(fs::temp_directory_path() /
                std::format("cind-project-{}",
                            std::chrono::steady_clock::now().time_since_epoch().count())) {
        fs::create_directories(path_);
    }
    ~TemporaryDirectory() { fs::remove_all(path_); }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << "content";
}

} // namespace

TEST_CASE("project discovery selects the closest marker above a file") {
    TemporaryDirectory temporary;
    touch(temporary.path() / "cmk.yaml");
    touch(temporary.path() / "nested" / ".git" / "HEAD");
    const fs::path file = temporary.path() / "nested" / "src" / "main.cpp";
    touch(file);

    const auto discovery = discover_project(file.string());
    REQUIRE(discovery.has_value());
    REQUIRE(discovery->has_value());
    CHECK((*discovery)->root == (temporary.path() / "nested").string());
    CHECK((*discovery)->marker == ".git");
}

TEST_CASE("project scanning returns stable files and watchable directories") {
    TemporaryDirectory temporary;
    touch(temporary.path() / "src" / "main.cpp");
    touch(temporary.path() / "include" / "main.hpp");
    touch(temporary.path() / "build" / "generated.cpp");

    const auto index = scan_project_files(temporary.path().string());
    REQUIRE(index.has_value());
    CHECK(index->files.size() == 2);
    CHECK(std::ranges::find(index->files, (temporary.path() / "src" / "main.cpp").string()) !=
          index->files.end());
    CHECK(std::ranges::find(index->directories, (temporary.path() / "src").string()) !=
          index->directories.end());
    CHECK(std::ranges::find(index->directories, (temporary.path() / "build").string()) ==
          index->directories.end());
}

TEST_CASE("project discovery and scanning acknowledge cancellation") {
    TemporaryDirectory temporary;
    touch(temporary.path() / "cmk.yaml");
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto discovery =
        discover_project((temporary.path() / "src" / "main.cpp").string(), cancelled.get_token());
    REQUIRE_FALSE(discovery.has_value());
    CHECK(discovery.error() == std::make_error_code(std::errc::operation_canceled));
    const auto index = scan_project_files(temporary.path().string(), 100, cancelled.get_token());
    REQUIRE_FALSE(index.has_value());
    CHECK(index.error() == std::make_error_code(std::errc::operation_canceled));
}
