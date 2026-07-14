#include "syntax/green_node.hpp"

#include "syntax/syntax_tree.hpp"

namespace cind {

namespace {

GreenRef encode(const std::vector<SyntaxNode>& nodes, SyntaxNodeId id) {
    const SyntaxNode& n = nodes[id];
    auto g = std::make_shared<GreenNode>();
    g->kind = n.kind;
    g->width = n.end_token - n.first_token; // MissingToken: first == end -> 0
    g->incomplete = n.incomplete;
    g->reclassified = n.reclassified;
    g->expected = n.expected;
    g->children.reserve(n.children.size());
    std::uint32_t cursor = n.first_token;
    for (SyntaxNodeId cid : n.children) {
        const SyntaxNode& c = nodes[cid];
        g->children.push_back(GreenChild{c.first_token - cursor, encode(nodes, cid)});
        cursor = c.end_token;
    }
    return g;
}

void materialize(const GreenRef& g, std::uint32_t base, SyntaxNodeId parent,
                 std::vector<SyntaxNode>& out) {
    const auto id = static_cast<SyntaxNodeId>(out.size());
    out.push_back(SyntaxNode{g->kind, base, base + g->width, parent, {}, g->incomplete,
                             g->reclassified, g->expected});
    std::vector<SyntaxNodeId> child_ids;
    child_ids.reserve(g->children.size());
    std::uint32_t cursor = base;
    for (const GreenChild& c : g->children) {
        const std::uint32_t cbase = cursor + c.leading;
        child_ids.push_back(static_cast<SyntaxNodeId>(out.size()));
        materialize(c.node, cbase, id, out);
        cursor = cbase + c.node->width;
    }
    out[id].children = std::move(child_ids);
}

} // namespace

GreenRef green_from_flat(const SyntaxTree& tree) {
    if (tree.nodes_.empty()) {
        return nullptr;
    }
    return encode(tree.nodes_, tree.root());
}

GreenRef green_from_flat_subtree(const std::vector<SyntaxNode>& nodes, SyntaxNodeId root) {
    if (nodes.empty()) {
        return nullptr;
    }
    return encode(nodes, root);
}

SyntaxTree flat_from_green(const GreenRef& root, std::vector<Token> tokens) {
    SyntaxTree tree;
    tree.green_root_ = root;
    tree.tokens_ = std::move(tokens);
    if (root) {
        materialize(root, 0, kInvalidNode, tree.nodes_);
    }
    return tree;
}

} // namespace cind
