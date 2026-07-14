#pragma once

#include "cpp_lexer/lexer_state.hpp"
#include "cpp_lexer/token.hpp"
#include "syntax/syntax_kind.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

class Text;
struct GreenNode;

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
    // BraceGroup reclassified to CompoundStatement on statement evidence:
    // its early tokens were consumed with brace-group (initializer-list)
    // semantics, so it is not a uniform statement container. Incremental
    // repair must not replay its item loop.
    bool reclassified = false;
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
    friend void reparse(SyntaxTree&, std::vector<LexerState>&, const Text&, const Text&,
                        std::span<const TextEdit>);
    friend SyntaxTree flat_from_green(const std::shared_ptr<const GreenNode>&, std::vector<Token>);

    std::vector<Token> tokens_;
    std::vector<SyntaxNode> nodes_;
};

struct LexOutput;

// Full parse. Never fails; always yields a root covering every token. The
// Text overload reads chunk by chunk without materializing the string.
SyntaxTree parse(std::string_view text);
SyntaxTree parse(const Text& text);
// Parse over an existing token stream (e.g. from an incremental relex);
// `lexed` must be the lex of `text`.
SyntaxTree parse(const Text& text, LexOutput lexed);

// In-place incremental reparse (design.md §17): advances `tree` (the parse of
// `old_text`, with `line_states` from its lex) to the parse of `new_text`,
// given the normalized edit list between the two. Internally: incremental
// relex, then either verbatim node reuse (token sequence unchanged), a
// bounded block reparse spliced into the node vector, or a full reparse as
// fallback. The result always equals parse(new_text) — fuzz-locked.
void reparse(SyntaxTree& tree, std::vector<LexerState>& line_states, const Text& old_text,
             const Text& new_text, std::span<const TextEdit> edits);

} // namespace cind
