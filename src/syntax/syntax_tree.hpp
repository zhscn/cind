#pragma once

#include "cpp_lexer/token.hpp"
#include "syntax/syntax_kind.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cind {

using SyntaxNodeId = std::uint32_t;
inline constexpr SyntaxNodeId kInvalidNode = 0xFFFFFFFFu;

// A node covers a half-open range of token indices. Tokens (including trivia)
// inside a node's range but outside all children belong to the node itself,
// so every token belongs to exactly one deepest node. Leading/trailing trivia
// of a construct stays with its parent. Nodes are immutable after parsing and
// store absolute coordinates (full reparse per edit; design.md decision 8).
struct SyntaxNode {
    SyntaxKind kind = SyntaxKind::Error;
    std::uint32_t first_token = 0;
    std::uint32_t end_token = 0;
    SyntaxNodeId parent = kInvalidNode;
    std::vector<SyntaxNodeId> children;
    bool incomplete = false;
    // MissingToken nodes only: what the parser expected here.
    TokenKind expected = TokenKind::EndOfFile;
};

class SyntaxTree {
public:
    SyntaxNodeId root() const { return 0; }
    const SyntaxNode& node(SyntaxNodeId id) const { return nodes_[id]; }
    std::size_t node_count() const { return nodes_.size(); }
    const std::vector<Token>& tokens() const { return tokens_; }

    // Absolute text range covered by the node (zero-length for MissingToken).
    TextRange node_range(SyntaxNodeId id) const;

    // Deepest node whose text range contains `offset` (root as fallback).
    SyntaxNodeId node_at(TextOffset offset) const;

    std::string dump(std::string_view text) const;

private:
    template <typename Source> friend class Parser;

    std::vector<Token> tokens_;
    std::vector<SyntaxNode> nodes_;
};

class Text;

// Full parse. Never fails; always yields a root covering every token. The
// Text overload reads chunk by chunk without materializing the string.
SyntaxTree parse(std::string_view text);
SyntaxTree parse(const Text& text);

} // namespace cind
