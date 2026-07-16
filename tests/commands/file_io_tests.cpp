#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "commands/file_io.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <stop_token>
#include <string>

using namespace cind;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "cind-file-io-XXXXXX").string();
        path_ = ::mkdtemp(pattern.data());
        if (path_.empty()) {
            throw std::runtime_error("mkdtemp failed");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("atomic save replaces content and preserves permissions") {
    TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "sample.cc";
    {
        std::ofstream output(path, std::ios::binary);
        output << "old";
    }
    REQUIRE(::chmod(path.c_str(), 0750) == 0);

    CHECK_FALSE(save_file_atomically(path, Text("new\ncontent\n")));
    CHECK(read_file(path) == "new\ncontent\n");

    struct stat status{};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    CHECK((status.st_mode & 07777) == 0750);
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory.path())) {
        CHECK_FALSE(entry.path().filename().string().starts_with(".cind-tmp-"));
    }
}

TEST_CASE("atomic save creates a new file and rejects symbolic-link destinations") {
    TemporaryDirectory directory;
    const std::filesystem::path created = directory.path() / "created.cc";
    CHECK_FALSE(save_file_atomically(created, Text("created")));
    CHECK(read_file(created) == "created");

    const std::filesystem::path target = directory.path() / "target.cc";
    const std::filesystem::path link = directory.path() / "link.cc";
    {
        std::ofstream output(target, std::ios::binary);
        output << "target";
    }
    REQUIRE(::symlink(target.c_str(), link.c_str()) == 0);

    const std::error_code error = save_file_atomically(link, Text("replacement"));
    CHECK(error == std::make_error_code(std::errc::too_many_symbolic_link_levels));
    CHECK(read_file(target) == "target");
    CHECK(std::filesystem::is_symlink(link));
}

TEST_CASE("file reads and directory listings expose worker-friendly value results") {
    TemporaryDirectory directory;
    const std::filesystem::path file = directory.path() / "sample.cc";
    const std::filesystem::path child = directory.path() / "src";
    std::filesystem::create_directory(child);
    {
        std::ofstream output(file, std::ios::binary);
        output << "contents";
    }

    const std::expected<FileReadResult, std::error_code> read = read_file_contents(file);
    REQUIRE(read.has_value());
    CHECK(read->exists);
    CHECK(read->contents == "contents");

    const std::expected<FileReadResult, std::error_code> missing =
        read_file_contents(directory.path() / "new.cc");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->exists);
    CHECK(missing->contents.empty());

    const std::expected<DirectoryListing, std::error_code> listing =
        list_directory(directory.path(), 100);
    REQUIRE(listing.has_value());
    CHECK(listing->directory == std::filesystem::absolute(directory.path()));
    CHECK(listing->entries.size() == 2);
    CHECK(std::ranges::any_of(listing->entries, [](const DirectoryEntry& entry) {
        return entry.name == "sample.cc" && !entry.directory;
    }));
    CHECK(std::ranges::any_of(listing->entries, [](const DirectoryEntry& entry) {
        return entry.name == "src" && entry.directory;
    }));
}

TEST_CASE("file operations acknowledge cancellation before external commit") {
    TemporaryDirectory directory;
    const std::filesystem::path path = directory.path() / "cancelled.cc";
    std::stop_source cancellation;
    REQUIRE(cancellation.request_stop());

    const std::expected<FileReadResult, std::error_code> read =
        read_file_contents(path, cancellation.get_token());
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == std::make_error_code(std::errc::operation_canceled));
    CHECK(save_file_atomically(path, Text("content"), cancellation.get_token()) ==
          std::make_error_code(std::errc::operation_canceled));
    CHECK_FALSE(std::filesystem::exists(path));
}
