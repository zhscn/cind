#include "editor/jump.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
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

bool JumpGraph::touch(JumpNodeId node) {
    JumpNode* target = find(node);
    if (target == nullptr) {
        return false;
    }
    target->last_visit = tick();
    return true;
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

std::vector<JumpNode> JumpGraph::evict(std::size_t maximum_nodes) {
    if (nodes_.size() <= maximum_nodes) {
        return {};
    }
    std::unordered_set<JumpNodeId> protected_nodes;
    for (const JumpEdge& edge : edges_) {
        if (edge.persistent) {
            protected_nodes.insert(edge.from);
            protected_nodes.insert(edge.to);
        }
    }
    std::vector<const JumpNode*> candidates;
    candidates.reserve(nodes_.size());
    for (const JumpNode& node : nodes_) {
        if (!protected_nodes.contains(node.id)) {
            candidates.push_back(&node);
        }
    }
    std::ranges::sort(candidates, [](const JumpNode* left, const JumpNode* right) {
        return std::tie(left->last_visit, left->created_at, left->id) <
               std::tie(right->last_visit, right->created_at, right->id);
    });
    const std::size_t count = std::min(nodes_.size() - maximum_nodes, candidates.size());
    std::unordered_set<JumpNodeId> removed_ids;
    std::vector<JumpNode> removed;
    removed_ids.reserve(count);
    removed.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        removed_ids.insert(candidates[index]->id);
        removed.push_back(*candidates[index]);
    }
    std::erase_if(edges_, [&](const JumpEdge& edge) {
        return removed_ids.contains(edge.from) || removed_ids.contains(edge.to);
    });
    std::erase_if(nodes_,
                  [&](const JumpNode& node) { return removed_ids.contains(node.id); });
    node_index_.clear();
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        node_index_.emplace(nodes_[index].id, index);
    }
    return removed;
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

void JumpWalk::forget(std::span<const JumpNodeId> nodes) {
    if (nodes.empty() || entries_.empty()) {
        return;
    }
    const std::unordered_set<JumpNodeId> removed(nodes.begin(), nodes.end());
    const std::size_t old_cursor = cursor_.value_or(entries_.size() - 1);
    std::vector<JumpNodeId> retained;
    retained.reserve(entries_.size());
    std::optional<std::size_t> retained_cursor;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (removed.contains(entries_[index])) {
            continue;
        }
        retained.push_back(entries_[index]);
        if (index <= old_cursor) {
            retained_cursor = retained.size() - 1;
        }
    }
    entries_ = std::move(retained);
    if (entries_.empty()) {
        cursor_.reset();
    } else {
        cursor_ = retained_cursor.value_or(0);
    }
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
