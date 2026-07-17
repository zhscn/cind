#pragma once

#include "editor/buffer.hpp"
#include "editor/ids.hpp"
#include "editor/settings.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

struct ProjectSpec {
    std::string name;
    std::vector<std::string> roots;
    std::string discovery_provider;
    std::string discovery_marker;
};

class Project {
public:
    ProjectId id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::vector<std::string>& roots() const { return roots_; }
    const std::string& discovery_provider() const { return discovery_provider_; }
    const std::string& discovery_marker() const { return discovery_marker_; }
    const std::vector<std::string>& files() const { return files_; }
    bool indexing() const { return indexing_; }
    std::uint64_t index_revision() const { return index_revision_; }
    const std::optional<std::string>& index_error() const { return index_error_; }
    SettingsLayer& settings() { return settings_; }
    const SettingsLayer& settings() const { return settings_; }

private:
    friend class ProjectRegistry;

    Project(ProjectId id, ProjectSpec spec, const SettingRegistry& settings)
        : id_(id), name_(std::move(spec.name)), roots_(std::move(spec.roots)),
          discovery_provider_(std::move(spec.discovery_provider)),
          discovery_marker_(std::move(spec.discovery_marker)),
          settings_(settings, SettingScope::Project) {}

    ProjectId id_;
    std::string name_;
    std::vector<std::string> roots_;
    std::string discovery_provider_;
    std::string discovery_marker_;
    std::vector<std::string> files_;
    bool indexing_ = false;
    std::uint64_t index_revision_ = 0;
    std::optional<std::string> index_error_;
    SettingsLayer settings_;
};

// Project is a tooling/configuration boundary, not a window-session owner.
// Discovery policy is intentionally outside this registry so native and
// scripted providers create the same validated ProjectSpec.
class ProjectRegistry {
public:
    ProjectRegistry(BufferRegistry& buffers, const SettingRegistry& settings)
        : buffers_(&buffers), settings_(&settings) {}

    ProjectId create(ProjectSpec spec);
    bool erase(ProjectId id);

    Project& get(ProjectId id);
    const Project& get(ProjectId id) const;
    Project* try_get(ProjectId id);
    const Project* try_get(ProjectId id) const;
    std::vector<ProjectId> all() const;

    void assign(BufferId buffer, std::optional<ProjectId> project);
    std::optional<ProjectId> find_by_root(std::string_view root) const;
    std::optional<ProjectId> find_for_resource(std::string_view uri) const;
    void begin_index(ProjectId project);
    void replace_index(ProjectId project, std::vector<std::string> files);
    void cancel_index(ProjectId project);
    void fail_index(ProjectId project, std::string error);

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<Project> value;
    };

    bool root_is_owned(std::string_view root) const;
    bool has_attached_buffers(ProjectId id) const;

    BufferRegistry* buffers_;
    const SettingRegistry* settings_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
};

} // namespace cind
