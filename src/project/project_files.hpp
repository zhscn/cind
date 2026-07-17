#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

namespace cind {

struct ProjectDiscoveryProvider {
    std::string name;
    std::vector<std::string> markers;

    friend bool operator==(const ProjectDiscoveryProvider&,
                           const ProjectDiscoveryProvider&) = default;
};

struct ProjectDiscovery {
    std::string root;
    std::string provider;
    std::string marker;
};

struct ProjectFileIndex {
    std::vector<std::string> files;
    std::vector<std::string> directories;
};

std::expected<std::optional<ProjectDiscovery>, std::error_code>
discover_project(std::string path, std::span<const ProjectDiscoveryProvider> providers,
                 const std::stop_token& cancellation = {});

std::expected<ProjectFileIndex, std::error_code>
scan_project_files(std::string root, std::size_t max_files = 200'000,
                   const std::stop_token& cancellation = {});

} // namespace cind
