#pragma once

#include "document/text.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace cind {

struct FileReadResult {
    bool exists = false;
    std::string contents;
};

struct DirectoryEntry {
    std::filesystem::path path;
    std::string name;
    bool directory = false;
};

struct DirectoryListing {
    std::filesystem::path directory;
    std::vector<DirectoryEntry> entries;
};

std::expected<FileReadResult, std::error_code>
read_file_contents(const std::filesystem::path& path, const std::stop_token& cancellation = {});

std::expected<DirectoryListing, std::error_code>
list_directory(const std::filesystem::path& path, std::size_t maximum_entries,
               const std::stop_token& cancellation = {});

// Writes a complete buffer through an exclusive same-directory temporary
// file, preserves an existing regular file's mode, then publishes it with
// rename and synchronizes the containing directory. Symbolic-link and
// non-regular destinations are rejected.
std::error_code save_file_atomically(const std::filesystem::path& path, const Text& content,
                                     const std::stop_token& cancellation = {});

} // namespace cind
