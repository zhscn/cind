#pragma once

#include "editor/ids.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace cind {

class ViewRegistry;

// A window is an application focus and display target. It binds one View,
// while the View owns buffer-relative caret, selection, and viewport state.
// Window-local keymaps are therefore suitable for display/focus behavior and
// never become implicit buffer state.
class Window {
public:
    WindowId id() const { return id_; }
    ViewId view_id() const { return view_id_; }
    std::vector<KeymapId>& keymaps() { return keymaps_; }
    const std::vector<KeymapId>& keymaps() const { return keymaps_; }

private:
    friend class WindowRegistry;

    Window(WindowId id, ViewId view) : id_(id), view_id_(view) {}

    WindowId id_;
    ViewId view_id_;
    std::vector<KeymapId> keymaps_;
};

class WindowRegistry {
public:
    explicit WindowRegistry(ViewRegistry& views) : views_(&views) {}
    ~WindowRegistry();

    WindowId create(ViewId view);
    bool erase(WindowId id);
    void set_view(WindowId window, ViewId view);

    Window& get(WindowId id);
    const Window& get(WindowId id) const;
    Window* try_get(WindowId id);
    const Window* try_get(WindowId id) const;
    std::vector<WindowId> all() const;

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<Window> value;
    };

    ViewRegistry* views_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
};

} // namespace cind
