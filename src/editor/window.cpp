#include "editor/window.hpp"

#include "editor/view.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cind {

namespace {

std::unique_ptr<WindowLayoutNode> make_leaf(WindowId window) {
    auto node = std::make_unique<WindowLayoutNode>();
    node->window = window;
    return node;
}

std::unique_ptr<WindowLayoutNode>* find_leaf(std::unique_ptr<WindowLayoutNode>& node,
                                             WindowId window) {
    if (!node) {
        return nullptr;
    }
    if (node->leaf()) {
        return node->window == window ? &node : nullptr;
    }
    if (std::unique_ptr<WindowLayoutNode>* found = find_leaf(node->first, window)) {
        return found;
    }
    return find_leaf(node->second, window);
}

void collect_leaves(const WindowLayoutNode* node, std::vector<WindowId>& leaves) {
    if (node == nullptr) {
        return;
    }
    if (node->leaf()) {
        leaves.push_back(node->window);
        return;
    }
    collect_leaves(node->first.get(), leaves);
    collect_leaves(node->second.get(), leaves);
}

bool erase_leaf(std::unique_ptr<WindowLayoutNode>& node, WindowId window) {
    if (!node || node->leaf()) {
        return false;
    }
    if (node->first && node->first->leaf() && node->first->window == window) {
        node = std::move(node->second);
        return true;
    }
    if (node->second && node->second->leaf() && node->second->window == window) {
        node = std::move(node->first);
        return true;
    }
    return erase_leaf(node->first, window) || erase_leaf(node->second, window);
}

} // namespace

WindowLayout::WindowLayout(WindowId root) : root_(make_leaf(root)), leaves_{root} {
    if (!root.valid()) {
        throw std::invalid_argument("window layout root is invalid");
    }
}

bool WindowLayout::contains(WindowId window) const {
    return std::ranges::find(leaves_, window) != leaves_.end();
}

bool WindowLayout::split(WindowId window, WindowId new_window, WindowSplitAxis axis, float ratio) {
    if (!new_window.valid() || contains(new_window) || !std::isfinite(ratio) || ratio <= 0.0F ||
        ratio >= 1.0F) {
        return false;
    }
    std::unique_ptr<WindowLayoutNode>* target = find_leaf(root_, window);
    if (target == nullptr) {
        return false;
    }
    auto branch = std::make_unique<WindowLayoutNode>();
    branch->axis = axis;
    branch->ratio = ratio;
    branch->first = std::move(*target);
    branch->second = make_leaf(new_window);
    *target = std::move(branch);
    rebuild_leaves();
    return true;
}

bool WindowLayout::erase(WindowId window) {
    if (leaves_.size() <= 1 || !erase_leaf(root_, window)) {
        return false;
    }
    rebuild_leaves();
    return true;
}

bool WindowLayout::retain(WindowId window) {
    if (!contains(window)) {
        return false;
    }
    root_ = make_leaf(window);
    rebuild_leaves();
    return true;
}

std::optional<WindowId> WindowLayout::next(WindowId window, int delta) const {
    if (leaves_.empty()) {
        return std::nullopt;
    }
    const auto found = std::ranges::find(leaves_, window);
    if (found == leaves_.end()) {
        return std::nullopt;
    }
    const auto size = static_cast<std::ptrdiff_t>(leaves_.size());
    const auto index = std::distance(leaves_.begin(), found);
    const auto wrapped = ((index + static_cast<std::ptrdiff_t>(delta)) % size + size) % size;
    return leaves_[static_cast<std::size_t>(wrapped)];
}

void WindowLayout::rebuild_leaves() {
    leaves_.clear();
    collect_leaves(root_.get(), leaves_);
}

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
