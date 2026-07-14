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

TypeCharResult type_char(Document& document, TextOffset caret, char ch,
                         const CppIndentStyle& style) {
    EditTransaction tx = document.begin_transaction();
    tx.insert(caret, std::string_view(&ch, 1));

    TypeCharResult result;
    result.caret = TextOffset{caret.value + 1};

    if (ch == '}' || ch == ':' || ch == '#') {
        DocumentSnapshot spec = tx.speculative_snapshot();
        const std::uint32_t line = spec.lines().position(caret).line;
        const TextOffset line_start = spec.lines().line_start(line);
        std::string_view prefix = spec.text(TextRange{line_start, caret});
        const bool first_content =
            prefix.find_first_not_of(" \t") == std::string_view::npos;

        // ':' may complete a label anywhere on the line; '}' and '#' only
        // reindent when they are the line's first content.
        if (ch == ':' || first_content) {
            SyntaxTree tree = parse(spec.text());
            IndentDecision decision = compute_line_indent(spec, tree, line, style);
            const bool colon_completes_label =
                decision.role == FormatRole::CaseLabel ||
                decision.role == FormatRole::AccessSpecifierLabel ||
                decision.role == FormatRole::ConstructorInitializerIntro;
            if (!decision.preserve && (ch != ':' || colon_completes_label)) {
                std::string_view current = leading_whitespace_of(spec, line);
                if (current != decision.indentation_text &&
                    caret.value >= line_start.value + current.size()) {
                    tx.replace(
                        TextRange{line_start,
                                  TextOffset{line_start.value +
                                             static_cast<std::uint32_t>(current.size())}},
                        decision.indentation_text);
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
    result.change = std::move(commit.change);
    return result;
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
