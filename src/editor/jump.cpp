#include "editor/jump.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cind {

JumpInternResult JumpGraph::intern(JumpPosition position) {
    if (!position.buffer && position.resource.empty()) {
        throw std::invalid_argument("jump position requires a buffer or resource");
    }
    if (position.buffer && position.anchor == 0) {
        throw std::invalid_argument("open jump position requires an anchor");
    }
    for (JumpNode& node : nodes_) {
        if (!same_location(node, position)) {
            continue;
        }
        const bool attach = !node.position.buffer && position.buffer;
        if (attach) {
            node.position = std::move(position);
        } else {
            node.position.fallback = position.fallback;
            if (!position.excerpt.empty()) {
                node.position.excerpt = std::move(position.excerpt);
            }
        }
        node.last_visit = tick();
        return {.node = node.id, .retained_position = attach};
    }
    if (next_node_ == std::numeric_limits<JumpNodeId>::max()) {
        throw std::overflow_error("jump node id space is exhausted");
    }
    const JumpNodeId id = ++next_node_;
    const std::uint64_t now = tick();
    node_index_.emplace(id, nodes_.size());
    nodes_.push_back(
        {.id = id, .position = std::move(position), .created_at = now, .last_visit = now});
    return {.node = id, .retained_position = true};
}

bool JumpGraph::link(JumpNodeId from, JumpNodeId to, std::string kind, bool persistent) {
    if (find(from) == nullptr || find(to) == nullptr || kind.empty()) {
        return false;
    }
    const auto existing = std::ranges::find_if(edges_, [&](const JumpEdge& edge) {
        return edge.from == from && edge.to == to && edge.kind == kind;
    });
    if (existing != edges_.end()) {
        existing->at = tick();
        existing->persistent = existing->persistent || persistent;
        return false;
    }
    edges_.push_back(
        {.from = from, .to = to, .kind = std::move(kind), .at = tick(), .persistent = persistent});
    return true;
}

const JumpNode* JumpGraph::find(JumpNodeId node) const {
    const auto found = node_index_.find(node);
    return found == node_index_.end() ? nullptr : &nodes_[found->second];
}

JumpNode* JumpGraph::find(JumpNodeId node) {
    return const_cast<JumpNode*>(std::as_const(*this).find(node));
}

std::vector<JumpEdge> JumpGraph::outgoing(JumpNodeId node) const {
    std::vector<JumpEdge> result;
    std::ranges::copy_if(edges_, std::back_inserter(result),
                         [node](const JumpEdge& edge) { return edge.from == node; });
    std::ranges::sort(result, std::greater{}, &JumpEdge::at);
    return result;
}

std::vector<JumpEdge> JumpGraph::incoming(JumpNodeId node) const {
    std::vector<JumpEdge> result;
    std::ranges::copy_if(edges_, std::back_inserter(result),
                         [node](const JumpEdge& edge) { return edge.to == node; });
    std::ranges::sort(result, std::greater{}, &JumpEdge::at);
    return result;
}

void JumpGraph::detach_buffer(BufferId buffer,
                              const std::function<LinePosition(AnchorId)>& resolve_position,
                              const std::function<void(AnchorId)>& remove_anchor) {
    for (JumpNode& node : nodes_) {
        if (node.position.buffer != buffer) {
            continue;
        }
        node.position.fallback = resolve_position(node.position.anchor);
        remove_anchor(node.position.anchor);
        node.position.buffer = {};
        node.position.anchor = 0;
    }
}

void JumpGraph::release_anchors(const std::function<void(BufferId, AnchorId)>& remove_anchor) {
    for (JumpNode& node : nodes_) {
        if (!node.position.buffer || node.position.anchor == 0) {
            continue;
        }
        remove_anchor(node.position.buffer, node.position.anchor);
        node.position.buffer = {};
        node.position.anchor = 0;
    }
}

bool JumpGraph::same_location(const JumpNode& node, const JumpPosition& position) const {
    const bool same_owner =
        (node.position.buffer && position.buffer && node.position.buffer == position.buffer) ||
        (!node.position.resource.empty() && node.position.resource == position.resource);
    return same_owner && node.position.fallback.line == position.fallback.line;
}

bool JumpWalk::record(JumpNodeId node) {
    if (node == kInvalidJumpNode) {
        return false;
    }
    if (!entries_.empty() && entries_.back() == node && cursor_ &&
        *cursor_ == entries_.size() - 1) {
        return false;
    }
    entries_.push_back(node);
    cursor_ = entries_.size() - 1;
    return true;
}

std::optional<JumpNodeId> JumpWalk::move(std::int64_t delta) {
    if (!cursor_ || entries_.empty() || delta == 0) {
        return std::nullopt;
    }
    const std::uint64_t current = *cursor_;
    const std::uint64_t maximum = entries_.size() - 1;
    std::uint64_t target = current;
    if (delta < 0) {
        const std::uint64_t distance = static_cast<std::uint64_t>(-(delta + 1)) + std::uint64_t{1};
        if (distance > current) {
            return std::nullopt;
        }
        target -= distance;
    } else if (const std::uint64_t distance = static_cast<std::uint64_t>(delta);
               distance > maximum - current) {
        return std::nullopt;
    } else {
        target += distance;
    }
    cursor_ = static_cast<std::size_t>(target);
    return entries_[*cursor_];
}

std::optional<JumpNodeId> JumpWalk::current() const {
    return cursor_ ? std::optional(entries_[*cursor_]) : std::nullopt;
}

void JumpWalk::clear() {
    entries_.clear();
    cursor_.reset();
}

std::string jump_edge_kind(std::string_view intent) {
    if (intent == "list") {
        return "list";
    }
    if (intent == "definition" || intent == "declaration" || intent == "implementation") {
        return "def";
    }
    if (intent == "reference" || intent == "references") {
        return "ref";
    }
    if (intent == "search") {
        return "search";
    }
    if (intent == "manual") {
        return "manual";
    }
    return "open";
}

} // namespace cind
