#pragma once

#include "cpp_lexer/lexer_state.hpp"
#include "cpp_lexer/token.hpp"
#include "syntax/green_node.hpp"
#include "syntax/syntax_kind.hpp"

#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

class Text;

// A red node: the position-aware view of one green node. It covers a half-open
// range of token indices; tokens (including trivia) inside its range but outside
// all children belong to the node itself, so every token belongs to exactly one
// deepest node. Leading/trailing trivia of a construct stays with its parent.
// Red nodes are computed lazily from the green tree on demand (design.md §607):
// `green`/`expanded` are the cache bookkeeping; all other fields are the
// consumer-visible view and are stable once the node exists.
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
    // Lazy-materialization bookkeeping (not part of the logical view).
    const GreenNode* green = nullptr; // the green node this red node mirrors
    bool expanded = false;            // children have been materialized
};

class SyntaxTree {
public:
    // Root red node id (always 0). Materializes the root on first access.
    SyntaxNodeId root() const;
    // The red node for `id` (must come from root()/node_at()/navigation).
    // Materializes its children on first access.
    const SyntaxNode& node(SyntaxNodeId id) const;
    // Total node count of the tree (walks the green tree; not the lazily
    // materialized red count).
    std::size_t node_count() const { return green_count(green_root_); }
    const std::vector<Token>& tokens() const { return tokens_; }

    // Absolute text range covered by the node (zero-length for MissingToken).
    TextRange node_range(SyntaxNodeId id) const;

    // Deepest node whose text range contains `offset` (root as fallback).
    SyntaxNodeId node_at(TextOffset offset) const;

    std::string dump(std::string_view text) const;

    // The length-encoded green tree — the source of truth from which red nodes
    // are materialized (design.md §607). Null only for a default-constructed tree.
    const GreenRef& green_root() const { return green_root_; }

private:
    template <typename Source> friend class Parser;
    friend void reparse(SyntaxTree&, std::vector<LexerState>&, const Text&, const Text&,
                        std::span<const TextEdit>);

    // Sets the green source of truth and drops any materialized red nodes.
    void set_green(GreenRef root) {
        green_root_ = std::move(root);
        red_.clear();
    }
    // Materializes id's children (one level) if not already done.
    void expand(SyntaxNodeId id) const;

    GreenRef green_root_;
    std::vector<Token> tokens_;
    // One-past-the-last token covered by any PPReopenedScope (0 if none).
    // Phantom scopes reshape the tree from #if-frame context that a block
    // repair cannot rebuild, and error recovery can stretch them past their
    // conditional's #endif — so reparse falls back to a full parse whenever
    // the damage window opens before this point.
    std::uint32_t pp_phantom_hi_ = 0;
    // Lazy red pool; index == SyntaxNodeId. deque so held `const SyntaxNode&`
    // stay valid as navigation materializes more nodes (no reallocation).
    mutable std::deque<SyntaxNode> red_;
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
