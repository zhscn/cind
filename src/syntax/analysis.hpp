#pragma once

#include "cpp_lexer/lexer.hpp"
#include "document/snapshot.hpp"
#include "syntax/syntax_tree.hpp"

#include <optional>
#include <span>

namespace cind {

// Derived lexical + syntactic structure of one revision's text. The tree owns
// the tokens; `line_states` completes the LexOutput needed to relex from
// here. `text` is the analyzed content (a persistent value — retaining it is
// cheap and it is what relex diffs against).
struct Analysis {
    RevisionId revision = 0;
    Text text;
    std::vector<LexerState> line_states;
    SyntaxTree tree;
};

// Revision-keyed memo of analyze() (design.md §6: syntax is a pure function
// of a snapshot; this class only caches it and advances the cache
// incrementally along edit lists). Not thread-safe; one per consumer.
class Analyzer {
public:
    // Analysis of `snap`: cached when revisions match, otherwise a full
    // lex + parse (use apply()/adopt() after edits to stay incremental).
    const Analysis& analyze(const DocumentSnapshot& snap);

    // Pure derivation for speculative snapshots: relex + reparse of
    // `new_text`, given its edit source `base`. Does not touch any cache.
    static Analysis derive(const Analysis& base, const Text& new_text,
                           std::span<const TextEdit> edits, RevisionId revision);

    // Advance the cache across a committed change (also undo/redo/undo_to).
    // Incremental when the cache sits at change.old_revision; otherwise the
    // cache is dropped and the next analyze() is a full pass.
    void apply(const DocumentChange& change, const DocumentSnapshot& after);

    // Install an already-computed analysis (e.g. the speculative analysis of
    // a transaction that was committed unchanged).
    void adopt(Analysis analysis);

    // Steals the cache when it sits at `revision` — the zero-copy path for
    // speculative advancement inside a transaction: take, reparse in place,
    // compute, and adopt() the result back at the commit revision.
    std::optional<Analysis> take(RevisionId revision);

    // One-shot full analysis (no cache involved).
    static Analysis full(const Text& text, RevisionId revision);

private:
    std::optional<Analysis> cache_;
};

} // namespace cind
