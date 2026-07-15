#include "editor/project.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace cind {

namespace {

std::string normalize_root(std::string root) {
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    return root;
}

bool contains_resource(std::string_view root, std::string_view uri) {
    if (!uri.starts_with(root)) {
        return false;
    }
    if (uri.size() == root.size() || root.ends_with('/')) {
        return true;
    }
    return uri[root.size()] == '/';
}

} // namespace

ProjectId ProjectRegistry::create(ProjectSpec spec) {
    if (spec.name.empty()) {
        throw std::invalid_argument("project name must not be empty");
    }
    if (spec.roots.empty()) {
        throw std::invalid_argument("project must have at least one root");
    }
    std::unordered_set<std::string> unique_roots;
    for (std::string& root : spec.roots) {
        root = normalize_root(std::move(root));
        if (root.empty()) {
            throw std::invalid_argument("project root must not be empty");
        }
        if (!unique_roots.insert(root).second) {
            throw std::invalid_argument("project contains a duplicate root");
        }
        if (root_is_owned(root)) {
            throw std::invalid_argument("project root is already owned");
        }
    }

    std::uint32_t slot_index = 0;
    if (free_slots_.empty()) {
        if (slots_.size() >= ProjectId::invalid_slot) {
            throw std::overflow_error("project registry is exhausted");
        }
        slot_index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    } else {
        slot_index = free_slots_.back();
        free_slots_.pop_back();
    }
    Slot& slot = slots_[slot_index];
    const ProjectId id{slot_index, slot.generation};
    slot.value = std::unique_ptr<Project>(new Project(id, std::move(spec), *settings_));
    return id;
}

bool ProjectRegistry::has_attached_buffers(ProjectId id) const {
    for (BufferId buffer : buffers_->all()) {
        if (buffers_->get(buffer).project_id() == id) {
            return true;
        }
    }
    return false;
}

bool ProjectRegistry::erase(ProjectId id) {
    Project* project = try_get(id);
    if (project == nullptr) {
        return false;
    }
    if (has_attached_buffers(id)) {
        return false;
    }
    Slot& slot = slots_[id.slot];
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
    free_slots_.push_back(id.slot);
    return true;
}

Project* ProjectRegistry::try_get(ProjectId id) {
    if (!id.valid() || id.slot >= slots_.size()) {
        return nullptr;
    }
    Slot& slot = slots_[id.slot];
    return slot.generation == id.generation ? slot.value.get() : nullptr;
}

const Project* ProjectRegistry::try_get(ProjectId id) const {
    return const_cast<ProjectRegistry*>(this)->try_get(id);
}

Project& ProjectRegistry::get(ProjectId id) {
    if (Project* project = try_get(id)) {
        return *project;
    }
    throw std::out_of_range("unknown or stale project id");
}

const Project& ProjectRegistry::get(ProjectId id) const {
    return const_cast<ProjectRegistry*>(this)->get(id);
}

std::vector<ProjectId> ProjectRegistry::all() const {
    std::vector<ProjectId> ids;
    ids.reserve(slots_.size() - free_slots_.size());
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].value) {
            ids.push_back(ProjectId{index, slots_[index].generation});
        }
    }
    return ids;
}

void ProjectRegistry::assign(BufferId buffer_id, std::optional<ProjectId> project_id) {
    Buffer& buffer = buffers_->get(buffer_id);
    if (project_id) {
        get(*project_id);
    }
    buffer.project_id_ = project_id;
}

bool ProjectRegistry::root_is_owned(std::string_view root) const {
    for (ProjectId id : all()) {
        const auto& roots = get(id).roots();
        if (std::ranges::find(roots, root) != roots.end()) {
            return true;
        }
    }
    return false;
}

std::optional<ProjectId> ProjectRegistry::find_for_resource(std::string_view uri) const {
    std::optional<ProjectId> best;
    std::size_t best_length = 0;
    for (ProjectId id : all()) {
        for (const std::string& root : get(id).roots()) {
            if (root.size() > best_length && contains_resource(root, uri)) {
                best = id;
                best_length = root.size();
            }
        }
    }
    return best;
}

} // namespace cind
