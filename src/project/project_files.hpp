#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace cind {

struct ProjectDiscovery {
    std::string root;
    std::string marker;
};

struct ProjectFileIndex {
    std::vector<std::string> files;
    std::vector<std::string> directories;
};

std::expected<std::optional<ProjectDiscovery>, std::error_code>
discover_project(std::string path, const std::stop_token& cancellation = {});

std::expected<ProjectFileIndex, std::error_code>
scan_project_files(std::string root, std::size_t max_files = 200'000,
                   const std::stop_token& cancellation = {});

} // namespace cind
