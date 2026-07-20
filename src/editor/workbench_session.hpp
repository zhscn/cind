#pragma once

#include "document/text_types.hpp"
#include "editor/window.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

struct WorkbenchWindowSessionState {
    std::optional<std::string> resource;
    std::uint32_t caret = 0;
    std::optional<std::string> role;
    bool pinned = false;
    bool created_by_policy = false;
    std::vector<std::uint64_t> jump_walk;
    std::optional<std::size_t> jump_cursor;
};

struct WorkbenchJumpNodeSessionState {
    std::uint64_t id = 0;
    std::string resource;
    LinePosition fallback;
    std::string excerpt;
    std::uint64_t created_at = 0;
    std::uint64_t last_visit = 0;
};

struct WorkbenchJumpEdgeSessionState {
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    std::string kind;
    std::uint64_t at = 0;
    bool persistent = false;
};

struct WorkbenchLayoutSessionState {
    std::optional<WorkbenchWindowSessionState> window;
    WindowSplitAxis axis = WindowSplitAxis::Rows;
    float ratio = 0.5F;
    std::unique_ptr<WorkbenchLayoutSessionState> first;
    std::unique_ptr<WorkbenchLayoutSessionState> second;

    bool leaf() const { return window.has_value(); }
};

struct WorkbenchSessionEntry {
    std::string name;
    std::vector<std::string> scope_roots;
    std::vector<std::string> mru_resources;
    WorkbenchLayoutSessionState layout;
    std::size_t active_leaf = 0;
    std::vector<WorkbenchJumpNodeSessionState> jump_nodes;
    std::vector<WorkbenchJumpEdgeSessionState> jump_edges;
};

struct WorkbenchSessionState {
    static constexpr std::uint32_t current_version = 2;

    std::uint32_t version = current_version;
    std::size_t active_workbench = 0;
    std::vector<WorkbenchSessionEntry> workbenches;
};

std::string serialize_workbench_session(const WorkbenchSessionState& state);
std::expected<WorkbenchSessionState, std::string>
parse_workbench_session(std::string_view serialized);

} // namespace cind
