#include "project/project_files.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

namespace cind {

namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 3> kProjectMarkers{
    ".git",
    "cmk.yaml",
    "compile_commands.json",
};

bool excluded_directory(std::string_view name) {
    constexpr std::array<std::string_view, 8> excluded{
        ".git", ".hg", ".svn", "node_modules", ".cache", "build", "out", "target",
    };
    return std::ranges::find(excluded, name) != excluded.end() || name.starts_with("build-") ||
           name.starts_with("cmake-build-");
}

std::error_code cancelled_error() {
    return std::make_error_code(std::errc::operation_canceled);
}

} // namespace

std::expected<std::optional<ProjectDiscovery>, std::error_code>
discover_project(std::string path, const std::stop_token& cancellation) {
    std::error_code error;
    fs::path current = fs::absolute(fs::path(std::move(path)), error).lexically_normal();
    if (error) {
        return std::unexpected(error);
    }
    current = current.parent_path();
    while (!current.empty()) {
        if (cancellation.stop_requested()) {
            return std::unexpected(cancelled_error());
        }
        for (const std::string_view marker : kProjectMarkers) {
            const bool exists = fs::exists(current / marker, error);
            if (error) {
                return std::unexpected(error);
            }
            if (exists) {
                return ProjectDiscovery{.root = current.string(), .marker = std::string(marker)};
            }
        }
        const fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

std::expected<ProjectFileIndex, std::error_code>
scan_project_files(std::string root, std::size_t max_files, const std::stop_token& cancellation) {
    ProjectFileIndex result;
    std::error_code error;
    fs::path normalized = fs::absolute(fs::path(std::move(root)), error).lexically_normal();
    if (error) {
        return std::unexpected(error);
    }
    if (!fs::is_directory(normalized, error)) {
        return std::unexpected(error ? error : std::make_error_code(std::errc::not_a_directory));
    }
    result.directories.push_back(normalized.string());
    fs::recursive_directory_iterator iterator(normalized,
                                              fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    if (error) {
        return std::unexpected(error);
    }
    while (iterator != end) {
        if (cancellation.stop_requested()) {
            return std::unexpected(cancelled_error());
        }
        const fs::directory_entry& entry = *iterator;
        const fs::file_status status = entry.symlink_status(error);
        if (error) {
            error.clear();
            iterator.increment(error);
            if (error) {
                error.clear();
            }
            continue;
        }
        if (fs::is_directory(status)) {
            if (excluded_directory(entry.path().filename().string()) || fs::is_symlink(status)) {
                iterator.disable_recursion_pending();
            } else {
                result.directories.push_back(entry.path().lexically_normal().string());
            }
        } else if (fs::is_regular_file(status)) {
            if (result.files.size() >= max_files) {
                return std::unexpected(std::make_error_code(std::errc::value_too_large));
            }
            result.files.push_back(entry.path().lexically_normal().string());
        }
        iterator.increment(error);
        if (error) {
            error.clear();
        }
    }
    std::ranges::sort(result.files);
    std::ranges::sort(result.directories);
    return result;
}

} // namespace cind
