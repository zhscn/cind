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
// entity pools. Guile owns selection, descriptive, membership, display and
// per-Window navigation policy; this object owns layout plus durable jump graph
// and transaction data, including anchors into native Buffer state.
class Workbench {
public:
    WorkbenchId id() const { return id_; }

    const WindowLayout& layout() const { return layout_; }
    WindowLayout& layout() { return layout_; }

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
    JumpGraph jumps_;
    LocationListStack location_lists_;
    TransactionGroupRegistry transaction_groups_;
};

// Generational application-local ownership for workbenches. Windows belong to
// at most one workbench layout; all layouts and their Window/View objects stay
// live independently of Guile's active selection.
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

    std::size_t size() const { return size_; }

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<Workbench> value;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::size_t size_ = 0;
};

} // namespace cind
