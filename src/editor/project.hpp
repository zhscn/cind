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
};

class Project {
public:
    ProjectId id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::vector<std::string>& roots() const { return roots_; }
    SettingsLayer& settings() { return settings_; }
    const SettingsLayer& settings() const { return settings_; }

private:
    friend class ProjectRegistry;

    Project(ProjectId id, ProjectSpec spec, const SettingRegistry& settings)
        : id_(id), name_(std::move(spec.name)), roots_(std::move(spec.roots)),
          settings_(settings, SettingScope::Project) {}

    ProjectId id_;
    std::string name_;
    std::vector<std::string> roots_;
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
    std::optional<ProjectId> find_for_resource(std::string_view uri) const;

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
