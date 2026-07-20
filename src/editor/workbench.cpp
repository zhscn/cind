#include "editor/workbench.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cind {

Workbench::Workbench(WorkbenchId id, WorkbenchSpec spec)
    : id_(id), layout_(spec.root_window), active_window_(spec.root_window) {
    if (!spec.root_window) {
        throw std::invalid_argument("workbench requires a root window");
    }
}

void Workbench::set_active_window(WindowId window) {
    if (!layout_.contains(window)) {
        throw std::invalid_argument("active window does not belong to the workbench");
    }
    active_window_ = window;
}

std::optional<WindowId> Workbench::slot(std::string_view role) const {
    const auto found = slots_.find(std::string(role));
    return found == slots_.end() ? std::nullopt : std::optional(found->second);
}

void Workbench::set_slot(std::string role, WindowId window) {
    if (role.empty() || !layout_.contains(window)) {
        throw std::invalid_argument("workbench slot requires a role and member window");
    }
    clear_window_slots(window);
    slots_.insert_or_assign(std::move(role), window);
}

void Workbench::clear_slot(std::string_view role) {
    slots_.erase(std::string(role));
}

void Workbench::clear_window_slots(WindowId window) {
    std::erase_if(slots_, [window](const auto& entry) { return entry.second == window; });
}

WorkbenchId WorkbenchRegistry::create(WorkbenchSpec spec) {
    if (find_by_window(spec.root_window)) {
        throw std::invalid_argument("workbench root window already belongs to a workbench");
    }
    std::uint32_t slot_index;
    if (free_slots_.empty()) {
        if (slots_.size() >= WorkbenchId::invalid_slot) {
            throw std::overflow_error("workbench registry is exhausted");
        }
        slot_index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    } else {
        slot_index = free_slots_.back();
        free_slots_.pop_back();
    }
    Slot& slot = slots_[slot_index];
    const WorkbenchId id{slot_index, slot.generation};
    try {
        slot.value = std::unique_ptr<Workbench>(new Workbench(id, spec));
    } catch (...) {
        free_slots_.push_back(slot_index);
        throw;
    }
    ++size_;
    if (!active_) {
        active_ = id;
    }
    return id;
}

bool WorkbenchRegistry::erase(WorkbenchId id) {
    Workbench* workbench = try_get(id);
    if (workbench == nullptr) {
        return false;
    }
    if (size_ == 1) {
        throw std::logic_error("cannot erase the last workbench");
    }
    if (active_ == id) {
        throw std::logic_error("cannot erase the active workbench");
    }
    Slot& slot = slots_[id.slot];
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
    free_slots_.push_back(id.slot);
    --size_;
    return true;
}

Workbench* WorkbenchRegistry::try_get(WorkbenchId id) {
    if (!id || id.slot >= slots_.size()) {
        return nullptr;
    }
    Slot& slot = slots_[id.slot];
    return slot.generation == id.generation ? slot.value.get() : nullptr;
}

const Workbench* WorkbenchRegistry::try_get(WorkbenchId id) const {
    return const_cast<WorkbenchRegistry*>(this)->try_get(id);
}

Workbench& WorkbenchRegistry::get(WorkbenchId id) {
    Workbench* result = try_get(id);
    if (result == nullptr) {
        throw std::out_of_range("unknown workbench id");
    }
    return *result;
}

const Workbench& WorkbenchRegistry::get(WorkbenchId id) const {
    return const_cast<WorkbenchRegistry*>(this)->get(id);
}

std::vector<WorkbenchId> WorkbenchRegistry::all() const {
    std::vector<WorkbenchId> result;
    result.reserve(size_);
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].value != nullptr) {
            result.push_back({index, slots_[index].generation});
        }
    }
    return result;
}

std::optional<WorkbenchId> WorkbenchRegistry::find_by_window(WindowId window) const {
    for (const WorkbenchId id : all()) {
        if (get(id).layout().contains(window)) {
            return id;
        }
    }
    return std::nullopt;
}

Workbench& WorkbenchRegistry::active() {
    if (!active_) {
        throw std::logic_error("workbench registry has no active workbench");
    }
    return get(active_);
}

const Workbench& WorkbenchRegistry::active() const {
    return const_cast<WorkbenchRegistry*>(this)->active();
}

bool WorkbenchRegistry::activate(WorkbenchId id) {
    if (try_get(id) == nullptr) {
        return false;
    }
    active_ = id;
    return true;
}

std::optional<WorkbenchId> WorkbenchRegistry::next(WorkbenchId id, int delta) const {
    const std::vector<WorkbenchId> ids = all();
    const auto found = std::ranges::find(ids, id);
    if (found == ids.end() || ids.empty()) {
        return std::nullopt;
    }
    const auto current = static_cast<std::ptrdiff_t>(std::distance(ids.begin(), found));
    const auto count = static_cast<std::ptrdiff_t>(ids.size());
    const auto wrapped = ((current + static_cast<std::ptrdiff_t>(delta)) % count + count) % count;
    return ids[static_cast<std::size_t>(wrapped)];
}

} // namespace cind
