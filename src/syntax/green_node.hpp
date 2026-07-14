#pragma once

#include "cpp_lexer/token.hpp"
#include "syntax/syntax_kind.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace cind {

class SyntaxTree;
struct SyntaxNode;

using SyntaxNodeId = std::uint32_t;
inline constexpr SyntaxNodeId kInvalidNode = 0xFFFFFFFFu;

// Length-encoded, position-independent, immutable syntax node (design.md §607,
// "green tree 的另一半"). A node records its total token span as a relative
// `width`; children carry a relative `leading` (the parent-owned tokens, trivia
// included, that precede them since the previous child or the node start).
// Leaf tokens are NOT green nodes — they stay implied by the token stream, so a
// green tree maps 1:1 onto the flat red SyntaxNode set and materializes
// byte-exact. Absolute positions are recovered by prefix-summing widths from the
// root; the token vector (absolute ranges) is held separately by SyntaxTree.
struct GreenNode;
using GreenRef = std::shared_ptr<const GreenNode>;

struct GreenChild {
    std::uint32_t leading; // parent-owned tokens before this child
    GreenRef node;
};

struct GreenNode {
    SyntaxKind kind = SyntaxKind::Error;
    std::uint32_t width = 0; // total tokens (incl. trivia) spanned; post-close-trim
    bool incomplete = false;
    bool reclassified = false;
    TokenKind expected = TokenKind::EndOfFile; // MissingToken only (width 0)
    std::vector<GreenChild> children;
};

// Encode one subtree of an arbitrary flat node vector (e.g. an incremental
// reparse sandbox result) as a green node, rooted at `root`.
GreenRef green_from_flat_subtree(const std::vector<SyntaxNode>& nodes, SyntaxNodeId root);

// Total green node count under (and including) `root`. O(nodes).
std::size_t green_count(const GreenRef& root);

// Deep structural + positional equality of two green trees: same kind, width,
// flags, expected, and children (with equal relative leadings) recursively. An
// id-free oracle — two trees are green_equal iff they materialize identically.
bool green_equal(const GreenRef& a, const GreenRef& b);

} // namespace cind
