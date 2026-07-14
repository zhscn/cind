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
    Analysis a;
    a.revision = revision;
    a.text = new_text;
    LexOutput new_lex = relex(base.tree.tokens(), base.line_states, base.text, new_text, edits);
    a.line_states = std::move(new_lex.line_states);
    new_lex.line_states.clear();
    a.tree = parse(new_text, std::move(new_lex));
    return a;
}

void Analyzer::apply(const DocumentChange& change, const DocumentSnapshot& after) {
    if (cache_ && cache_->revision == change.old_revision &&
        after.revision() == change.new_revision) {
        cache_ = derive(*cache_, after.content(), change.edits, change.new_revision);
        return;
    }
    cache_.reset();
}

void Analyzer::adopt(Analysis analysis) { cache_ = std::move(analysis); }

} // namespace cind
