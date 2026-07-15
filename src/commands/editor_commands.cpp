#include "commands/editor_commands.hpp"

#include <algorithm>

namespace cind {

namespace {

std::string leading_whitespace_of(const DocumentSnapshot& snapshot, std::uint32_t line) {
    TextRange content = snapshot.content().line_content_range(line);
    std::string s = snapshot.substring(content);
    std::size_t n = 0;
    while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) {
        ++n;
    }
    s.resize(n);
    return s;
}

// EnterBetweenBraces predicate: the nearest significant tokens around the
// caret are '{' and its matching '}' of the same syntax node, with only
// single-line trivia in between (a '}' already on its own line just needs the
// fallback). Matching is CST-based, so unbalanced braces simply fail the
// predicate and fall through to plain newline-and-indent.
bool between_braces(const DocumentSnapshot& snapshot, const SyntaxTree& tree, TextOffset caret) {
    const auto& tokens = tree.tokens();
    // First token starting at or after the caret. The stream is contiguous
    // and sorted, so only the token before `at` can straddle the caret, and
    // every non-trivia token before it ends at or before the caret.
    auto it = std::ranges::lower_bound(tokens, caret, {},
                                       [](const Token& t) { return t.range.start; });
    const auto at = static_cast<std::size_t>(it - tokens.begin());
    if (at > 0) {
        const Token t = tokens[at - 1];
        if (t.range.start < caret && caret < t.range.end) {
            return false; // caret inside a token (comment, literal, ...)
        }
    }
    std::size_t prev = tokens.size();
    for (std::size_t j = at; j-- > 0;) {
        if (!is_trivia(tokens[j].kind)) {
            prev = j;
            break;
        }
    }
    std::size_t next = tokens.size();
    for (std::size_t j = at; j < tokens.size(); ++j) {
        const Token t = tokens[j];
        if (t.kind == TokenKind::EndOfFile) {
            break;
        }
        if (!is_trivia(t.kind)) {
            next = j;
            break;
        }
    }
    if (prev == tokens.size() || next == tokens.size() || prev >= next) {
        return false;
    }
    if (tokens[prev].kind != TokenKind::LBrace || tokens[next].kind != TokenKind::RBrace) {
        return false;
    }
    for (std::size_t i = prev + 1; i < next; ++i) {
        if (!is_trivia(tokens[i].kind)) {
            return false;
        }
        const TextRange r = tokens[i].range;
        if (snapshot.substring(r).contains('\n')) {
            return false;
        }
    }
    // Same node owns both braces: its range starts at '{' and ends at '}'.
    SyntaxNodeId owner = tree.node_at(tokens[prev].range.start);
    switch (tree.node(owner).kind) {
    case SyntaxKind::CompoundStatement:
    case SyntaxKind::BraceGroup:
    case SyntaxKind::NamespaceBody:
    case SyntaxKind::ClassBody: break;
    default: return false;
    }
    TextRange owner_range = tree.node_range(owner);
    return owner_range.end == tokens[next].range.end;
}

// Steals the analyzer cache and advances it in place onto the transaction's
// pending state — the zero-copy speculative analysis. Falls back to a full
// pass when the cache was cold.
Analysis speculative_analysis(Analyzer& analyzer, const EditTransaction& tx,
                              const Text& old_text, RevisionId old_revision,
                              const DocumentSnapshot& spec) {
    if (std::optional<Analysis> stolen = analyzer.take(old_revision)) {
        reparse(stolen->tree, stolen->line_states, old_text, spec.content(), tx.pending_edits());
        stolen->text = spec.content();
        stolen->revision = spec.revision();
        return std::move(*stolen);
    }
    return Analyzer::full(spec.content(), spec.revision());
}

// Advances a speculative analysis over edits made after it was computed
// (given in speculative coordinates), then hands it to the analyzer as the
// committed state.
void adopt_committed(Analyzer& analyzer, Analysis analysis, std::vector<TextEdit> late_edits,
                     const CommitResult& commit) {
    const Text spec_text = analysis.text;
    reparse(analysis.tree, analysis.line_states, spec_text, commit.snapshot.content(),
            late_edits);
    analysis.text = commit.snapshot.content();
    analysis.revision = commit.snapshot.revision();
    analyzer.adopt(std::move(analysis));
}

EnterResult enter_between_braces(Document& document, TextOffset caret,
                                 const CppIndentStyle& style, Analyzer& analyzer,
                                 const Text& old_text, RevisionId old_revision) {
    EditTransaction tx = document.begin_transaction();
    tx.insert(caret, "\n\n");

    DocumentSnapshot spec = tx.speculative_snapshot();
    Analysis spec_analysis = speculative_analysis(analyzer, tx, old_text, old_revision, spec);
    const std::uint32_t caret_line = spec.content().position(caret).line;

    IndentDecision middle = compute_line_indent(spec, spec_analysis.tree, caret_line + 1, style);
    IndentDecision closing = compute_line_indent(spec, spec_analysis.tree, caret_line + 2, style);
    middle.trace.insert(middle.trace.begin(), "enter handler: EnterBetweenBraces");

    // Higher offset first so the earlier insert does not shift it.
    const TextOffset closing_start = spec.content().line_start(caret_line + 2);
    tx.insert(closing_start, closing.indentation_text);
    TextOffset middle_start = spec.content().line_start(caret_line + 1);
    tx.insert(middle_start, middle.indentation_text);

    std::vector<TextEdit> late;
    late.push_back(TextEdit{TextRange{middle_start, middle_start}, middle.indentation_text});
    late.push_back(TextEdit{TextRange{closing_start, closing_start}, closing.indentation_text});

    TextOffset final_caret{middle_start.value +
                           static_cast<std::uint32_t>(middle.indentation_text.size())};
    CommitResult commit = tx.commit();
    adopt_committed(analyzer, std::move(spec_analysis), std::move(late), commit);
    return EnterResult{"EnterBetweenBraces", std::move(middle), final_caret,
                       std::move(commit.change)};
}

EnterResult newline_and_indent(Document& document, TextOffset caret, const CppIndentStyle& style,
                               Analyzer& analyzer, const Text& old_text,
                               RevisionId old_revision) {
    EditTransaction tx = document.begin_transaction();
    tx.insert(caret, "\n");

    DocumentSnapshot spec = tx.speculative_snapshot();
    Analysis spec_analysis = speculative_analysis(analyzer, tx, old_text, old_revision, spec);
    const std::uint32_t new_line = spec.content().position(TextOffset{caret.value + 1}).line;

    IndentDecision decision = compute_line_indent(spec, spec_analysis.tree, new_line, style);
    decision.trace.insert(decision.trace.begin(), "enter handler: NewlineAndIndent (fallback)");

    std::string ws = decision.indentation_text;
    if (decision.preserve) {
        // Never write into a raw string; inside a block comment continue the
        // previous line's leading whitespace.
        ws = decision.role == FormatRole::PreservedBlockComment
                 ? leading_whitespace_of(spec, new_line - 1)
                 : std::string();
    }
    const TextOffset ws_at{caret.value + 1};
    tx.insert(ws_at, ws);

    std::vector<TextEdit> late;
    if (!ws.empty()) {
        late.push_back(TextEdit{TextRange{ws_at, ws_at}, ws});
    }

    TextOffset final_caret{caret.value + 1 + static_cast<std::uint32_t>(ws.size())};
    CommitResult commit = tx.commit();
    adopt_committed(analyzer, std::move(spec_analysis), std::move(late), commit);
    return EnterResult{"NewlineAndIndent", std::move(decision), final_caret,
                       std::move(commit.change)};
}

} // namespace

EnterResult press_enter(Document& document, TextOffset caret, const CppIndentStyle& style,
                        Analyzer& analyzer) {
    DocumentSnapshot snapshot = document.snapshot();
    const Analysis& base = analyzer.analyze(snapshot);
    const bool braces = between_braces(snapshot, base.tree, caret);
    if (braces) {
        return enter_between_braces(document, caret, style, analyzer, snapshot.content(),
                                    snapshot.revision());
    }
    return newline_and_indent(document, caret, style, analyzer, snapshot.content(),
                              snapshot.revision());
}

EnterResult press_enter(Document& document, TextOffset caret, const CppIndentStyle& style) {
    Analyzer analyzer;
    return press_enter(document, caret, style, analyzer);
}

TypeCharResult type_char(Document& document, TextOffset caret, char ch,
                         const CppIndentStyle& style, Analyzer& analyzer) {
    const Text old_text = document.snapshot().content();
    const RevisionId old_revision = document.revision();
    EditTransaction tx = document.begin_transaction();
    tx.insert(caret, std::string_view(&ch, 1));

    TypeCharResult result;
    result.caret = TextOffset{caret.value + 1};

    std::optional<Analysis> spec_analysis;
    std::vector<TextEdit> late;
    if (ch == '}' || ch == ':' || ch == '#') {
        DocumentSnapshot spec = tx.speculative_snapshot();
        const std::uint32_t line = spec.content().position(caret).line;
        const TextOffset line_start = spec.content().line_start(line);
        const std::string prefix = spec.substring(TextRange{line_start, caret});
        const bool first_content = prefix.find_first_not_of(" \t") == std::string::npos;

        // ':' may complete a label anywhere on the line; '}' and '#' only
        // reindent when they are the line's first content.
        if (ch == ':' || first_content) {
            spec_analysis = speculative_analysis(analyzer, tx, old_text, old_revision, spec);
            IndentDecision decision = compute_line_indent(spec, spec_analysis->tree, line, style);
            const bool colon_completes_label =
                decision.role == FormatRole::CaseLabel ||
                decision.role == FormatRole::AccessSpecifierLabel ||
                decision.role == FormatRole::ConstructorInitializerIntro;
            if (!decision.preserve && (ch != ':' || colon_completes_label)) {
                const std::string current = leading_whitespace_of(spec, line);
                if (current != decision.indentation_text &&
                    caret.value >= line_start.value + current.size()) {
                    const TextRange ws_range{
                        line_start,
                        TextOffset{line_start.value + static_cast<std::uint32_t>(current.size())}};
                    tx.replace(ws_range, decision.indentation_text);
                    late.push_back(TextEdit{ws_range, decision.indentation_text});
                    result.caret.value +=
                        static_cast<std::uint32_t>(decision.indentation_text.size()) -
                        static_cast<std::uint32_t>(current.size());
                    decision.trace.insert(decision.trace.begin(),
                                          "typed-char handler: reindent on input");
                    result.reindented = true;
                    result.decision = std::move(decision);
                }
            }
        }
    }

    CommitResult commit = tx.commit();
    if (spec_analysis) {
        adopt_committed(analyzer, std::move(*spec_analysis), std::move(late), commit);
    } else {
        analyzer.apply(commit.change, commit.snapshot);
    }
    result.change = std::move(commit.change);
    return result;
}

TypeCharResult type_char(Document& document, TextOffset caret, char ch,
                         const CppIndentStyle& style) {
    Analyzer analyzer;
    return type_char(document, caret, ch, style, analyzer);
}

IndentDecision indent_line(Document& document, std::uint32_t line, const CppIndentStyle& style,
                           Analyzer& analyzer) {
    DocumentSnapshot snapshot = document.snapshot();
    const Analysis& base = analyzer.analyze(snapshot);
    IndentDecision decision = compute_line_indent(snapshot, base.tree, line, style);
    if (decision.preserve) {
        return decision;
    }
    const std::string current = leading_whitespace_of(snapshot, line);
    if (current != decision.indentation_text) {
        TextOffset start = snapshot.content().line_start(line);
        EditTransaction tx = document.begin_transaction();
        tx.replace(TextRange{start, TextOffset{start.value +
                                               static_cast<std::uint32_t>(current.size())}},
                   decision.indentation_text);
        CommitResult commit = tx.commit();
        analyzer.apply(commit.change, commit.snapshot);
    }
    return decision;
}

IndentDecision indent_line(Document& document, std::uint32_t line, const CppIndentStyle& style) {
    Analyzer analyzer;
    return indent_line(document, line, style, analyzer);
}

} // namespace cind
