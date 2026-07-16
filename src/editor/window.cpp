#include "editor/window.hpp"

#include "editor/view.hpp"

#include <stdexcept>

namespace cind {

WindowRegistry::~WindowRegistry() {
    for (Slot& slot : slots_) {
        if (slot.value) {
            --views_->get(slot.value->view_id_).attached_windows_;
        }
    }
}

WindowId WindowRegistry::create(ViewId view) {
    View& attached_view = views_->get(view);
    std::uint32_t slot_index = 0;
    if (free_slots_.empty()) {
        if (slots_.size() >= WindowId::invalid_slot) {
            throw std::overflow_error("window registry is exhausted");
        }
        slot_index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    } else {
        slot_index = free_slots_.back();
        free_slots_.pop_back();
    }
    Slot& slot = slots_[slot_index];
    const WindowId id{slot_index, slot.generation};
    slot.value = std::unique_ptr<Window>(new Window(id, view));
    ++attached_view.attached_windows_;
    return id;
}

bool WindowRegistry::erase(WindowId id) {
    if (try_get(id) == nullptr) {
        return false;
    }
    Slot& slot = slots_[id.slot];
    --views_->get(slot.value->view_id_).attached_windows_;
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
    free_slots_.push_back(id.slot);
    return true;
}

void WindowRegistry::set_view(WindowId window, ViewId view) {
    View& replacement = views_->get(view);
    Window& target = get(window);
    if (target.view_id_ == view) {
        return;
    }
    --views_->get(target.view_id_).attached_windows_;
    target.view_id_ = view;
    ++replacement.attached_windows_;
}

Window* WindowRegistry::try_get(WindowId id) {
    if (!id.valid() || id.slot >= slots_.size()) {
        return nullptr;
    }
    Slot& slot = slots_[id.slot];
    return slot.generation == id.generation ? slot.value.get() : nullptr;
}

const Window* WindowRegistry::try_get(WindowId id) const {
    return const_cast<WindowRegistry*>(this)->try_get(id);
}

Window& WindowRegistry::get(WindowId id) {
    if (Window* window = try_get(id)) {
        return *window;
    }
    throw std::out_of_range("unknown or stale window id");
}

const Window& WindowRegistry::get(WindowId id) const {
    return const_cast<WindowRegistry*>(this)->get(id);
}

std::vector<WindowId> WindowRegistry::all() const {
    std::vector<WindowId> ids;
    ids.reserve(slots_.size() - free_slots_.size());
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].value) {
            ids.push_back(WindowId{index, slots_[index].generation});
        }
    }
    return ids;
}

} // namespace cind
