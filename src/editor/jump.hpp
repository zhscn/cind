#pragma once

#include "document/anchor.hpp"
#include "document/text_types.hpp"
#include "editor/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

using JumpNodeId = std::uint64_t;
inline constexpr JumpNodeId kInvalidJumpNode = 0;

struct JumpPosition {
    BufferId buffer;
    std::string resource;
    AnchorId anchor = 0;
    LinePosition fallback;
    std::string excerpt;
};

struct JumpNode {
    JumpNodeId id = kInvalidJumpNode;
    JumpPosition position;
    std::uint64_t created_at = 0;
    std::uint64_t last_visit = 0;
};

struct JumpEdge {
    JumpNodeId from = kInvalidJumpNode;
    JumpNodeId to = kInvalidJumpNode;
    std::string kind;
    std::uint64_t at = 0;
    bool persistent = false;
};

struct JumpInternResult {
    JumpNodeId node = kInvalidJumpNode;
    bool retained_position = false;
};

class JumpGraph {
public:
    JumpInternResult intern(JumpPosition position);
    bool link(JumpNodeId from, JumpNodeId to, std::string kind, bool persistent = false);

    const JumpNode* find(JumpNodeId node) const;
    JumpNode* find(JumpNodeId node);
    bool touch(JumpNodeId node);
    std::span<const JumpNode> nodes() const { return nodes_; }
    std::span<const JumpEdge> edges() const { return edges_; }
    std::vector<JumpEdge> outgoing(JumpNodeId node) const;
    std::vector<JumpEdge> incoming(JumpNodeId node) const;
    std::vector<JumpNode> evict(std::size_t maximum_nodes);
    void restore(std::vector<JumpNode> nodes, std::vector<JumpEdge> edges);

    void detach_buffer(BufferId buffer,
                       const std::function<LinePosition(AnchorId)>& resolve_position,
                       const std::function<void(AnchorId)>& remove_anchor);
    void attach_buffer(
        std::string_view resource, BufferId buffer,
        const std::function<std::pair<AnchorId, LinePosition>(const JumpPosition&)>& resolve);
    void release_anchors(const std::function<void(BufferId, AnchorId)>& remove_anchor);

private:
    bool same_location(const JumpNode& node, const JumpPosition& position) const;
    std::uint64_t tick() { return ++clock_; }

    std::vector<JumpNode> nodes_;
    std::vector<JumpEdge> edges_;
    std::unordered_map<JumpNodeId, std::size_t> node_index_;
    JumpNodeId next_node_ = 0;
    std::uint64_t clock_ = 0;
};

class JumpWalk {
public:
    bool record(JumpNodeId node);
    std::optional<JumpNodeId> move(std::int64_t delta);
    std::optional<JumpNodeId> current() const;
    std::span<const JumpNodeId> entries() const { return entries_; }
    std::optional<std::size_t> cursor() const { return cursor_; }
    void forget(std::span<const JumpNodeId> nodes);
    void restore(std::vector<JumpNodeId> entries, std::optional<std::size_t> cursor);
    void clear();

private:
    std::vector<JumpNodeId> entries_;
    std::optional<std::size_t> cursor_;
};

std::string jump_edge_kind(std::string_view intent);

} // namespace cind
