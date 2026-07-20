#pragma once

#include "editor/ids.hpp"
#include "editor/jump.hpp"
#include "editor/location_list.hpp"
#include "editor/transaction_group.hpp"
#include "editor/window.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cind {

struct WorkbenchSpec {
    WindowId root_window;
};

// Native workbench data binds a window layout over the application's global
// entity pools. Guile owns descriptive, membership and display metadata; this
// object owns layout, focus and navigation data.
class Workbench {
public:
    WorkbenchId id() const { return id_; }

    const WindowLayout& layout() const { return layout_; }
    WindowLayout& layout() { return layout_; }
    WindowId active_window() const { return active_window_; }
    void set_active_window(WindowId window);

    JumpGraph& jumps() { return jumps_; }
    const JumpGraph& jumps() const { return jumps_; }
    LocationListStack& location_lists() { return location_lists_; }
    const LocationListStack& location_lists() const { return location_lists_; }
    TransactionGroupRegistry& transaction_groups() { return transaction_groups_; }
    const TransactionGroupRegistry& transaction_groups() const { return transaction_groups_; }

private:
    friend class WorkbenchRegistry;

    Workbench(WorkbenchId id, WorkbenchSpec spec);

    WorkbenchId id_;
    WindowLayout layout_;
    WindowId active_window_;
    JumpGraph jumps_;
    LocationListStack location_lists_;
    TransactionGroupRegistry transaction_groups_;
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
    std::optional<WorkbenchId> find_by_window(WindowId window) const;

    WorkbenchId active_id() const { return active_; }
    Workbench& active();
    const Workbench& active() const;
    bool activate(WorkbenchId id);
    std::optional<WorkbenchId> next(WorkbenchId id, int delta = 1) const;

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
