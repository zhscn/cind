#pragma once

#include "editor/ids.hpp"
#include "editor/jump.hpp"
#include "editor/location_list.hpp"
#include "editor/window.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cind {

struct WorkbenchSpec {
    std::string name;
    WindowId root_window;
    std::vector<ProjectId> scope;
};

// A workbench is one durable editing surface over the application's global
// Buffer and Project pools. It owns presentation/focus state, declared
// project visibility and the recency footprint accumulated by displaying
// Buffers. It does not own Buffer contents or project tooling state.
class Workbench {
public:
    WorkbenchId id() const { return id_; }
    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    const WindowLayout& layout() const { return layout_; }
    WindowLayout& layout() { return layout_; }
    WindowId active_window() const { return active_window_; }
    void set_active_window(WindowId window);

    std::span<const ProjectId> scope() const { return scope_; }
    bool contains_project(ProjectId project) const;
    bool adopt_project(ProjectId project);
    bool expel_project(ProjectId project);

    std::span<const BufferId> mru() const { return mru_; }
    bool contains_buffer(BufferId buffer) const;
    void visit_buffer(BufferId buffer);
    bool expel_buffer(BufferId buffer);
    void replace_mru(const std::vector<BufferId>& buffers);

    const std::unordered_map<std::string, WindowId>& slots() const { return slots_; }
    std::optional<WindowId> slot(std::string_view role) const;
    void set_slot(std::string role, WindowId window);
    void clear_slot(std::string_view role);
    void clear_window_slots(WindowId window);

    JumpGraph& jumps() { return jumps_; }
    const JumpGraph& jumps() const { return jumps_; }
    LocationListStack& location_lists() { return location_lists_; }
    const LocationListStack& location_lists() const { return location_lists_; }

private:
    friend class WorkbenchRegistry;

    Workbench(WorkbenchId id, WorkbenchSpec spec);

    WorkbenchId id_;
    std::string name_;
    std::vector<ProjectId> scope_;
    WindowLayout layout_;
    WindowId active_window_;
    std::vector<BufferId> mru_;
    std::unordered_map<std::string, WindowId> slots_;
    JumpGraph jumps_;
    LocationListStack location_lists_;
};

// Generational application-local ownership for workbenches. Exactly one
// workbench is active whenever the registry is non-empty. Windows belong to
// at most one workbench layout; non-active layouts and their Window/View
// objects stay live.
class WorkbenchRegistry {
public:
    WorkbenchId create(WorkbenchSpec spec);
    bool erase(WorkbenchId id);

    Workbench& get(WorkbenchId id);
    const Workbench& get(WorkbenchId id) const;
    Workbench* try_get(WorkbenchId id);
    const Workbench* try_get(WorkbenchId id) const;
    std::vector<WorkbenchId> all() const;
    std::optional<WorkbenchId> find_by_name(std::string_view name) const;
    std::optional<WorkbenchId> find_by_window(WindowId window) const;
    bool rename(WorkbenchId id, std::string name);

    WorkbenchId active_id() const { return active_; }
    Workbench& active();
    const Workbench& active() const;
    bool activate(WorkbenchId id);
    std::optional<WorkbenchId> next(WorkbenchId id, int delta = 1) const;

    void forget_buffer(BufferId buffer);
    void forget_project(ProjectId project);
    std::size_t size() const { return size_; }

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<Workbench> value;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    WorkbenchId active_;
    std::size_t size_ = 0;
};

} // namespace cind
