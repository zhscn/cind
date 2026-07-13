#include "commands/editor_commands.hpp"

namespace cind {

namespace {

std::string_view leading_whitespace_of(const DocumentSnapshot& snapshot, std::uint32_t line) {
    TextRange content = snapshot.lines().line_content_range(line);
    std::string_view s = snapshot.text(content);
    std::size_t n = 0;
    while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) {
        ++n;
    }
    return s.substr(0, n);
}

// EnterBetweenBraces predicate: the nearest significant tokens around the
// caret are '{' and its matching '}' of the same syntax node, with only
// single-line trivia in between (a '}' already on its own line just needs the
// fallback). Matching is CST-based, so unbalanced braces simply fail the
// predicate and fall through to plain newline-and-indent.
bool between_braces(std::string_view text, const SyntaxTree& tree, TextOffset caret) {
    const auto& tokens = tree.tokens();
    std::size_t prev = tokens.size();
    std::size_t next = tokens.size();
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.kind == TokenKind::EndOfFile) {
            break;
        }
        if (t.range.start < caret && caret < t.range.end) {
            return false; // caret inside a token (comment, literal, ...)
        }
        if (!is_trivia(t.kind) && t.range.end <= caret) {
            prev = i;
        }
        if (!is_trivia(t.kind) && next == tokens.size() && t.range.start >= caret) {
            next = i;
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
        if (text.substr(r.start.value, r.length()).contains('\n')) {
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

EnterResult enter_between_braces(Document& document, TextOffset caret,
                                 const CppIndentStyle& style) {
    EditTransaction tx = document.begin_transaction();
    tx.insert(caret, "\n\n");

    DocumentSnapshot spec = tx.speculative_snapshot();
    SyntaxTree tree = parse(spec.text());
    const std::uint32_t caret_line = spec.lines().position(caret).line;

    IndentDecision middle = compute_line_indent(spec, tree, caret_line + 1, style);
    IndentDecision closing = compute_line_indent(spec, tree, caret_line + 2, style);
    middle.trace.insert(middle.trace.begin(), "enter handler: EnterBetweenBraces");

    // Higher offset first so the earlier insert does not shift it.
    tx.insert(spec.lines().line_start(caret_line + 2), closing.indentation_text);
    TextOffset middle_start = spec.lines().line_start(caret_line + 1);
    tx.insert(middle_start, middle.indentation_text);

    TextOffset final_caret{middle_start.value +
                           static_cast<std::uint32_t>(middle.indentation_text.size())};
    CommitResult commit = tx.commit();
    return EnterResult{"EnterBetweenBraces", std::move(middle), final_caret,
                       std::move(commit.change)};
}

EnterResult newline_and_indent(Document& document, TextOffset caret,
                               const CppIndentStyle& style) {
    EditTransaction tx = document.begin_transaction();
    tx.insert(caret, "\n");

    DocumentSnapshot spec = tx.speculative_snapshot();
    SyntaxTree tree = parse(spec.text());
    const std::uint32_t new_line = spec.lines().position(TextOffset{caret.value + 1}).line;

    IndentDecision decision = compute_line_indent(spec, tree, new_line, style);
    decision.trace.insert(decision.trace.begin(), "enter handler: NewlineAndIndent (fallback)");

    std::string ws = decision.indentation_text;
    if (decision.preserve) {
        // Never write into a raw string; inside a block comment continue the
        // previous line's leading whitespace.
        ws = decision.role == FormatRole::PreservedBlockComment
                 ? std::string(leading_whitespace_of(spec, new_line - 1))
                 : std::string();
    }
    tx.insert(TextOffset{caret.value + 1}, ws);

    TextOffset final_caret{caret.value + 1 + static_cast<std::uint32_t>(ws.size())};
    CommitResult commit = tx.commit();
    return EnterResult{"NewlineAndIndent", std::move(decision), final_caret,
                       std::move(commit.change)};
}

} // namespace

EnterResult press_enter(Document& document, TextOffset caret, const CppIndentStyle& style) {
    DocumentSnapshot snapshot = document.snapshot();
    SyntaxTree tree = parse(snapshot.text());
    if (between_braces(snapshot.text(), tree, caret)) {
        return enter_between_braces(document, caret, style);
    }
    return newline_and_indent(document, caret, style);
}

IndentDecision indent_line(Document& document, std::uint32_t line, const CppIndentStyle& style) {
    DocumentSnapshot snapshot = document.snapshot();
    SyntaxTree tree = parse(snapshot.text());
    IndentDecision decision = compute_line_indent(snapshot, tree, line, style);
    if (decision.preserve) {
        return decision;
    }
    std::string_view current = leading_whitespace_of(snapshot, line);
    if (current != decision.indentation_text) {
        TextOffset start = snapshot.lines().line_start(line);
        EditTransaction tx = document.begin_transaction();
        tx.replace(TextRange{start, TextOffset{start.value +
                                               static_cast<std::uint32_t>(current.size())}},
                   decision.indentation_text);
        tx.commit();
    }
    return decision;
}

} // namespace cind
