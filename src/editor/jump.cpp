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
    std::erase_if(nodes_, [&](const JumpNode& node) { return removed_ids.contains(node.id); });
    node_index_.clear();
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        node_index_.emplace(nodes_[index].id, index);
    }
    return removed;
}

void JumpGraph::restore(std::vector<JumpNode> nodes, std::vector<JumpEdge> edges) {
    if (!nodes_.empty() || !edges_.empty()) {
        throw std::logic_error("jump graph restore requires an empty graph");
    }
    std::unordered_set<JumpNodeId> ids;
    JumpNodeId next = 0;
    std::uint64_t clock = 0;
    for (const JumpNode& node : nodes) {
        if (node.id == kInvalidJumpNode || node.position.resource.empty() || node.position.buffer ||
            node.position.anchor != 0 || !ids.insert(node.id).second) {
            throw std::invalid_argument("restored jump node is invalid");
        }
        next = std::max(next, node.id);
        clock = std::max({clock, node.created_at, node.last_visit});
    }
    for (const JumpEdge& edge : edges) {
        if (!ids.contains(edge.from) || !ids.contains(edge.to) || edge.kind.empty()) {
            throw std::invalid_argument("restored jump edge is invalid");
        }
        clock = std::max(clock, edge.at);
    }
    nodes_ = std::move(nodes);
    edges_ = std::move(edges);
    node_index_.clear();
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        node_index_.emplace(nodes_[index].id, index);
    }
    next_node_ = next;
    clock_ = clock;
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

void JumpGraph::attach_buffer(
    std::string_view resource, BufferId buffer,
    const std::function<std::pair<AnchorId, LinePosition>(const JumpPosition&)>& resolve) {
    if (!buffer || resource.empty()) {
        throw std::invalid_argument("jump buffer attachment requires a resource and buffer");
    }
    for (JumpNode& node : nodes_) {
        if (node.position.buffer || node.position.resource != resource) {
            continue;
        }
        auto [anchor, fallback] = resolve(node.position);
        if (anchor == 0) {
            throw std::logic_error("jump buffer attachment produced an invalid anchor");
        }
        node.position.buffer = buffer;
        node.position.anchor = anchor;
        node.position.fallback = fallback;
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

} // namespace cind
