#pragma once

#include "cpp_lexer/token.hpp"
#include "syntax/syntax_kind.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace cind {

class SyntaxTree;

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

// Encode a flat red tree as a green tree (relative length encoding). Reads only
// the public tree API. Returns nullptr for an empty tree.
GreenRef green_from_flat(const SyntaxTree& tree);

// Materialize a flat SyntaxTree from a green root and its token stream — the
// inverse of green_from_flat. Ids are assigned in DFS-preorder, exactly as the
// parser allocates them, so the round-trip is byte-exact (this is also the
// Phase-2 red-cache materializer).
SyntaxTree flat_from_green(const GreenRef& root, std::vector<Token> tokens);

} // namespace cind
