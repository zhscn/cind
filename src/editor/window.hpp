#pragma once

#include "editor/ids.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

class ViewRegistry;

enum class WindowSplitAxis : std::uint8_t {
    Rows,
    Columns,
};

// Persistent frontend-independent pane topology. Leaves are editor windows;
// branches divide their available extent between two children. Frontends may
// map the normalized ratio to cells or pixels without owning editor state.
struct WindowLayoutNode {
    WindowId window;
    WindowSplitAxis axis = WindowSplitAxis::Rows;
    float ratio = 0.5F;
    std::unique_ptr<WindowLayoutNode> first;
    std::unique_ptr<WindowLayoutNode> second;

    bool leaf() const { return window.valid(); }
};

struct WindowLayoutRect {
    int row = 0;
    int column = 0;
    int rows = 0;
    int columns = 0;
};

struct WindowPlacement {
    WindowId window;
    WindowLayoutRect rect;
};

struct WindowDivider {
    WindowSplitAxis axis = WindowSplitAxis::Rows;
    int position = 0;
    int start = 0;
    int length = 0;
};

struct WindowPartition {
    std::vector<WindowPlacement> windows;
    std::vector<WindowDivider> dividers;
};

struct WindowSplitSpec {
    WindowId target;
    WindowId new_window;
    WindowSplitAxis axis = WindowSplitAxis::Rows;
    float ratio = 0.5F;
};

class WindowLayout {
public:
    WindowLayout() = default;
    explicit WindowLayout(WindowId root);

    const WindowLayoutNode* root() const { return root_.get(); }
    std::span<const WindowId> leaves() const { return leaves_; }
    bool contains(WindowId window) const;

    bool split(const WindowSplitSpec& spec);
    bool erase(WindowId window);
    bool retain(WindowId window);
    std::optional<WindowId> next(WindowId window, int delta = 1) const;
    WindowPartition partition(int rows, int columns) const;

private:
    void rebuild_leaves();

    std::unique_ptr<WindowLayoutNode> root_;
    std::vector<WindowId> leaves_;
};

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
    const std::optional<std::string>& role() const { return role_; }
    void set_role(std::optional<std::string> role);
    bool pinned() const { return pinned_; }
    void set_pinned(bool pinned) { pinned_ = pinned; }
    bool created_by_policy() const { return created_by_policy_; }
    void set_created_by_policy(bool created) { created_by_policy_ = created; }

private:
    friend class WindowRegistry;

    Window(WindowId id, ViewId view) : id_(id), view_id_(view) {}

    WindowId id_;
    ViewId view_id_;
    std::vector<KeymapId> keymaps_;
    std::optional<std::string> role_;
    bool pinned_ = false;
    bool created_by_policy_ = false;
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
