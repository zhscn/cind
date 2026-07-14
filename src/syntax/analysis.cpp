#include "syntax/analysis.hpp"

namespace cind {

namespace {

Analysis full_analysis(const Text& text, RevisionId revision) {
    Analysis a;
    a.revision = revision;
    a.text = text;
    LexOutput lexed = lex(text);
    a.line_states = std::move(lexed.line_states);
    lexed.line_states.clear();
    a.tree = parse(text, std::move(lexed));
    return a;
}

} // namespace

const Analysis& Analyzer::analyze(const DocumentSnapshot& snap) {
    if (!cache_ || cache_->revision != snap.revision()) {
        cache_ = full_analysis(snap.content(), snap.revision());
    }
    return *cache_;
}

Analysis Analyzer::derive(const Analysis& base, const Text& new_text,
                          std::span<const TextEdit> edits, RevisionId revision) {
    Analysis a = base; // O(n) vector copies; the reparse below is incremental
    reparse(a.tree, a.line_states, base.text, new_text, edits);
    a.text = new_text;
    a.revision = revision;
    return a;
}

void Analyzer::apply(const DocumentChange& change, const DocumentSnapshot& after) {
    if (cache_ && cache_->revision == change.old_revision &&
        after.revision() == change.new_revision) {
        // Fully in place: no copies of the token or node vectors.
        reparse(cache_->tree, cache_->line_states, cache_->text, after.content(), change.edits);
        cache_->text = after.content();
        cache_->revision = change.new_revision;
        return;
    }
    cache_.reset();
}

void Analyzer::adopt(Analysis analysis) { cache_ = std::move(analysis); }

std::optional<Analysis> Analyzer::take(RevisionId revision) {
    if (!cache_ || cache_->revision != revision) {
        return std::nullopt;
    }
    std::optional<Analysis> out = std::move(cache_);
    cache_.reset();
    return out;
}

Analysis Analyzer::full(const Text& text, RevisionId revision) {
    return full_analysis(text, revision);
}

} // namespace cind
