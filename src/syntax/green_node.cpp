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

} // namespace

GreenRef green_from_flat_subtree(const std::vector<SyntaxNode>& nodes, SyntaxNodeId root) {
    if (nodes.empty()) {
        return nullptr;
    }
    return encode(nodes, root);
}

std::size_t green_count(const GreenRef& root) {
    if (!root) {
        return 0;
    }
    std::size_t n = 1;
    for (const GreenChild& c : root->children) {
        n += green_count(c.node);
    }
    return n;
}

bool green_equal(const GreenRef& a, const GreenRef& b) {
    if (a == b) {
        return true; // same pointer (shared subtree) or both null
    }
    if (!a || !b) {
        return false;
    }
    if (a->kind != b->kind || a->width != b->width || a->incomplete != b->incomplete ||
        a->reclassified != b->reclassified || a->expected != b->expected ||
        a->children.size() != b->children.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a->children.size(); ++i) {
        if (a->children[i].leading != b->children[i].leading ||
            !green_equal(a->children[i].node, b->children[i].node)) {
            return false;
        }
    }
    return true;
}

} // namespace cind
