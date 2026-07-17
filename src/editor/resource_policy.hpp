#pragma once

#include "editor/mode.hpp"
#include "project/project_files.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

struct FileModeRule {
    std::string name;
    ModeId mode;
    std::vector<std::string> suffixes;
    std::vector<std::string> filenames;

    friend bool operator==(const FileModeRule&, const FileModeRule&) = default;
};

// Stores declarative resource policy separately from filesystem I/O and
// Buffer/Project lifecycle. Later definitions have precedence, allowing a
// user extension to replace bundled policy without replacing native
// mechanisms.
class ResourcePolicyRegistry {
public:
    explicit ResourcePolicyRegistry(const ModeRegistry& modes) : modes_(&modes) {}

    void define_file_mode(std::string name, ModeId mode, std::vector<std::string> suffixes,
                          std::vector<std::string> filenames = {});
    void define_project_provider(std::string name, std::vector<std::string> markers);

    std::optional<ModeId> mode_for(std::string_view resource) const;
    const std::vector<FileModeRule>& file_mode_rules() const { return file_mode_rules_; }
    const std::vector<ProjectDiscoveryProvider>& project_providers() const {
        return project_providers_;
    }

    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

private:
    const ModeRegistry* modes_;
    std::vector<FileModeRule> file_mode_rules_;
    std::vector<ProjectDiscoveryProvider> project_providers_;
    bool sealed_ = false;
};

} // namespace cind
