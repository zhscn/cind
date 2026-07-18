#include "editor/resource_policy.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cind {

namespace {

void require_name(std::string_view name, std::string_view kind) {
    if (name.empty()) {
        throw std::invalid_argument(std::format("{} name must not be empty", kind));
    }
}

void require_unique_nonempty(const std::vector<std::string>& values, std::string_view kind) {
    std::unordered_set<std::string> unique;
    for (const std::string& value : values) {
        if (value.empty()) {
            throw std::invalid_argument(std::format("{} must not be empty", kind));
        }
        if (!unique.insert(value).second) {
            throw std::invalid_argument(std::format("duplicate {} '{}'", kind, value));
        }
    }
}

bool valid_component(std::string_view value) {
    const std::filesystem::path path(value);
    return value != "." && value != ".." && path.filename() == path;
}

template <typename Definition>
void replace_named(std::vector<Definition>& definitions, Definition definition) {
    std::erase_if(definitions,
                  [&](const Definition& existing) { return existing.name == definition.name; });
    definitions.push_back(std::move(definition));
}

} // namespace

std::expected<std::string, std::string> normalize_resource_path(std::string_view input) {
    if (input.empty()) {
        return std::unexpected("file path is empty");
    }
    std::error_code error;
    const std::filesystem::path path =
        std::filesystem::absolute(std::filesystem::path(input), error).lexically_normal();
    if (error) {
        return std::unexpected(std::format("invalid path: {}", error.message()));
    }
    return path.string();
}

void ResourcePolicyRegistry::define_file_mode(std::string name, ModeId mode,
                                              std::vector<std::string> suffixes,
                                              std::vector<std::string> filenames) {
    if (sealed_) {
        throw std::logic_error("resource policy registry is sealed");
    }
    require_name(name, "file mode rule");
    if (modes_->definition(mode).kind != ModeKind::Major) {
        throw std::invalid_argument("file mode rule must reference a major mode");
    }
    if (suffixes.empty() && filenames.empty()) {
        throw std::invalid_argument("file mode rule requires a suffix or filename");
    }
    require_unique_nonempty(suffixes, "file suffix");
    require_unique_nonempty(filenames, "filename");
    if (std::ranges::any_of(filenames,
                            [](const std::string& value) { return !valid_component(value); })) {
        throw std::invalid_argument("file mode rule filenames must be path components");
    }
    replace_named(file_mode_rules_, {.name = std::move(name),
                                     .mode = mode,
                                     .suffixes = std::move(suffixes),
                                     .filenames = std::move(filenames)});
}

void ResourcePolicyRegistry::define_project_provider(std::string name,
                                                     std::vector<std::string> markers) {
    if (sealed_) {
        throw std::logic_error("resource policy registry is sealed");
    }
    require_name(name, "project provider");
    if (markers.empty()) {
        throw std::invalid_argument("project provider requires at least one marker");
    }
    require_unique_nonempty(markers, "project marker");
    if (std::ranges::any_of(markers,
                            [](const std::string& value) { return !valid_component(value); })) {
        throw std::invalid_argument("project markers must be path components");
    }
    replace_named(project_providers_, {.name = std::move(name), .markers = std::move(markers)});
}

std::optional<ModeId> ResourcePolicyRegistry::mode_for(std::string_view resource) const {
    const std::string filename = std::filesystem::path(resource).filename().string();
    for (const FileModeRule& rule : std::views::reverse(file_mode_rules_)) {
        if (std::ranges::find(rule.filenames, filename) != rule.filenames.end()) {
            return rule.mode;
        }
        if (std::ranges::any_of(rule.suffixes, [&](const std::string& suffix) {
                return filename.size() >= suffix.size() && filename.ends_with(suffix);
            })) {
            return rule.mode;
        }
    }
    return std::nullopt;
}

} // namespace cind
