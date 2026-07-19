#include "editor/scheme_mode.hpp"

#include "commands/editor_commands.hpp"
#include "cpp_lexer/token_buffer.hpp"
#include "document/document.hpp"
#include "editor/language_mechanism.hpp"
#include "syntax/analysis.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

namespace {

enum class DatumTokenKind : std::uint8_t { Atom, Open, Close, Prefix };

struct DatumToken {
    DatumTokenKind kind = DatumTokenKind::Atom;
    std::size_t start = 0;
    std::size_t end = 0;
    std::optional<std::size_t> pair;
};

struct SchemeScan {
    std::vector<DatumToken> datums;
    std::vector<Token> tokens;
};

bool is_space(char byte) {
    return std::isspace(static_cast<unsigned char>(byte)) != 0;
}

bool is_open(char byte) {
    return byte == '(' || byte == '[' || byte == '{';
}

bool is_close(char byte) {
    return byte == ')' || byte == ']' || byte == '}';
}

bool matches(char open, char close) {
    return (open == '(' && close == ')') || (open == '[' && close == ']') ||
           (open == '{' && close == '}');
}

char matching_close(char open) {
    switch (open) {
    case '(':
        return ')';
    case '[':
        return ']';
    case '{':
        return '}';
    default:
        return '\0';
    }
}

TokenKind bracket_kind(char byte) {
    switch (byte) {
    case '(':
        return TokenKind::LParen;
    case ')':
        return TokenKind::RParen;
    case '[':
        return TokenKind::LBracket;
    case ']':
        return TokenKind::RBracket;
    case '{':
        return TokenKind::LBrace;
    case '}':
        return TokenKind::RBrace;
    default:
        return TokenKind::Invalid;
    }
}

bool is_atom_delimiter(char byte) {
    return is_space(byte) || is_open(byte) || is_close(byte) || byte == ';' || byte == '"' ||
           byte == '|' || byte == '\'' || byte == '`' || byte == ',';
}

std::size_t quoted_end(std::string_view text, std::size_t at, char quote, bool& terminated) {
    ++at;
    bool escaped = false;
    while (at < text.size()) {
        const char byte = text[at++];
        if (escaped) {
            escaped = false;
        } else if (byte == '\\') {
            escaped = true;
        } else if (byte == quote) {
            terminated = true;
            break;
        }
    }
    return at;
}

std::size_t character_end(std::string_view text, std::size_t at) {
    at += 2;
    if (at == text.size()) {
        return at;
    }
    ++at;
    while (at < text.size() && !is_atom_delimiter(text[at])) {
        ++at;
    }
    return at;
}

bool is_number(std::string_view atom) {
    if (atom.empty()) {
        return false;
    }
    std::size_t at = (atom.front() == '+' || atom.front() == '-') ? 1 : 0;
    bool digit = false;
    for (; at < atom.size(); ++at) {
        const char byte = atom[at];
        if (std::isdigit(static_cast<unsigned char>(byte)) != 0) {
            digit = true;
        } else if (byte != '.' && byte != '/' && byte != 'e' && byte != 'E' && byte != '#') {
            return false;
        }
    }
    return digit;
}

bool is_keyword(std::string_view atom) {
    constexpr std::array keywords{"and",
                                  "begin",
                                  "case",
                                  "cond",
                                  "define",
                                  "define*",
                                  "define-module",
                                  "define-public",
                                  "define-syntax",
                                  "do",
                                  "else",
                                  "if",
                                  "lambda",
                                  "let",
                                  "let*",
                                  "letrec",
                                  "letrec*",
                                  "match",
                                  "or",
                                  "parameterize",
                                  "quasiquote",
                                  "quote",
                                  "set!",
                                  "syntax-case",
                                  "syntax-rules",
                                  "unless",
                                  "unquote",
                                  "unquote-splicing",
                                  "when",
                                  "with-exception-handler"};
    return std::ranges::find(keywords, atom) != keywords.end();
}

void push_token(SchemeScan& scan, TokenKind kind, std::size_t start, std::size_t end,
                LexicalFlags flags = LexicalFlags::None) {
    scan.tokens.push_back(Token{kind,
                                TextRange{TextOffset{static_cast<std::uint32_t>(start)},
                                          TextOffset{static_cast<std::uint32_t>(end)}},
                                flags});
}

SchemeScan scan_scheme(std::string_view text) {
    SchemeScan scan;
    std::vector<std::size_t> opens;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t start = at;
        if (text[at] == '\n') {
            ++at;
            push_token(scan, TokenKind::Newline, start, at);
            continue;
        }
        if (is_space(text[at])) {
            while (at < text.size() && text[at] != '\n' && is_space(text[at])) {
                ++at;
            }
            push_token(scan, TokenKind::Whitespace, start, at);
            continue;
        }
        if (text[at] == ';') {
            while (at < text.size() && text[at] != '\n') {
                ++at;
            }
            push_token(scan, TokenKind::LineComment, start, at);
            continue;
        }
        if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '|') {
            std::size_t depth = 1;
            at += 2;
            while (at < text.size() && depth != 0) {
                if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '|') {
                    ++depth;
                    at += 2;
                } else if (at + 1 < text.size() && text[at] == '|' && text[at + 1] == '#') {
                    --depth;
                    at += 2;
                } else {
                    ++at;
                }
            }
            push_token(scan, TokenKind::BlockComment, start, at,
                       depth == 0 ? LexicalFlags::None : LexicalFlags::Unterminated);
            continue;
        }
        if (text[at] == '"' || text[at] == '|') {
            const char quote = text[at];
            bool terminated = false;
            at = quoted_end(text, at, quote, terminated);
            push_token(scan, quote == '"' ? TokenKind::StringLiteral : TokenKind::Identifier, start,
                       at, terminated ? LexicalFlags::None : LexicalFlags::Unterminated);
            scan.datums.push_back({DatumTokenKind::Atom, start, at, std::nullopt});
            continue;
        }
        if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '\\') {
            at = character_end(text, at);
            push_token(scan, TokenKind::CharacterLiteral, start, at);
            scan.datums.push_back({DatumTokenKind::Atom, start, at, std::nullopt});
            continue;
        }
        if (is_open(text[at])) {
            const std::size_t index = scan.datums.size();
            scan.datums.push_back({DatumTokenKind::Open, at, at + 1, std::nullopt});
            opens.push_back(index);
            ++at;
            push_token(scan, bracket_kind(text[start]), start, at);
            continue;
        }
        if (is_close(text[at])) {
            const std::size_t index = scan.datums.size();
            scan.datums.push_back({DatumTokenKind::Close, at, at + 1, std::nullopt});
            if (!opens.empty() && matches(text[scan.datums[opens.back()].start], text[at])) {
                const std::size_t open = opens.back();
                opens.pop_back();
                scan.datums[open].pair = index;
                scan.datums[index].pair = open;
            }
            ++at;
            push_token(scan, bracket_kind(text[start]), start, at);
            continue;
        }
        if (text[at] == '\'' || text[at] == '`' || text[at] == ',') {
            ++at;
            if (text[start] == ',' && at < text.size() && text[at] == '@') {
                ++at;
            }
            push_token(scan, TokenKind::Punctuator, start, at);
            scan.datums.push_back({DatumTokenKind::Prefix, start, at, std::nullopt});
            continue;
        }
        if (at + 1 < text.size() && text[at] == '#' &&
            (text[at + 1] == ';' || text[at + 1] == '\'' || text[at + 1] == '`' ||
             text[at + 1] == ',')) {
            at += 2;
            if (text[start + 1] == ',' && at < text.size() && text[at] == '@') {
                ++at;
            }
            push_token(scan, TokenKind::Punctuator, start, at);
            scan.datums.push_back({DatumTokenKind::Prefix, start, at, std::nullopt});
            continue;
        }
        if (text[at] == '#') {
            std::size_t marker_end = at + 1;
            while (marker_end < text.size() &&
                   std::isalnum(static_cast<unsigned char>(text[marker_end])) != 0) {
                ++marker_end;
            }
            if (marker_end < text.size() && is_open(text[marker_end])) {
                push_token(scan, TokenKind::Punctuator, start, marker_end);
                scan.datums.push_back({DatumTokenKind::Prefix, start, marker_end, std::nullopt});
                at = marker_end;
                continue;
            }
        }

        while (at < text.size() && !is_atom_delimiter(text[at])) {
            if (at + 1 < text.size() && text[at] == '#' && text[at + 1] == '|') {
                break;
            }
            ++at;
        }
        if (at == start) {
            ++at;
        }
        const std::string_view atom = text.substr(start, at - start);
        push_token(scan,
                   is_keyword(atom) ? TokenKind::IfKw
                                    : (is_number(atom) ? TokenKind::Number : TokenKind::Identifier),
                   start, at);
        scan.datums.push_back({DatumTokenKind::Atom, start, at, std::nullopt});
    }
    push_token(scan, TokenKind::EndOfFile, text.size(), text.size());
    return scan;
}

Analysis analyze_scheme(const DocumentSnapshot& snapshot) {
    Text text = snapshot.content();
    SchemeScan scan = scan_scheme(text.to_string());
    std::vector<LexerState> states(text.line_count());
    return {.revision = snapshot.revision(),
            .text = text,
            .line_states = std::move(states),
            .tree = parse(text, TokenBuffer(scan.tokens))};
}

std::optional<std::size_t> forward_token(const std::vector<DatumToken>& tokens, std::size_t index) {
    while (index < tokens.size() && tokens[index].kind == DatumTokenKind::Prefix) {
        ++index;
    }
    if (index == tokens.size() || tokens[index].kind == DatumTokenKind::Close) {
        return std::nullopt;
    }
    if (tokens[index].kind == DatumTokenKind::Open) {
        return tokens[index].pair;
    }
    return index;
}

std::optional<TextOffset> forward_datum(const std::vector<DatumToken>& tokens, std::size_t from) {
    const auto found =
        std::ranges::find_if(tokens, [from](const DatumToken& token) { return token.end > from; });
    if (found == tokens.end()) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(found - tokens.begin());
    const std::optional<std::size_t> end = forward_token(tokens, index);
    return end ? std::optional<TextOffset>{TextOffset{static_cast<std::uint32_t>(tokens[*end].end)}}
               : std::nullopt;
}

std::optional<TextOffset> backward_datum(const std::vector<DatumToken>& tokens, std::size_t from) {
    std::optional<std::size_t> found;
    for (std::size_t index = tokens.size(); index > 0; --index) {
        if (tokens[index - 1].start < from) {
            found = index - 1;
            break;
        }
    }
    if (!found) {
        return std::nullopt;
    }
    std::size_t index = *found;
    if (tokens[index].kind == DatumTokenKind::Close) {
        const std::size_t pair = tokens[index].pair.value_or(tokens.size());
        if (pair == tokens.size()) {
            return std::nullopt;
        }
        index = pair;
    } else if (tokens[index].kind == DatumTokenKind::Open ||
               tokens[index].kind == DatumTokenKind::Prefix) {
        return std::nullopt;
    }
    while (index > 0 && tokens[index - 1].kind == DatumTokenKind::Prefix) {
        --index;
    }
    return TextOffset{static_cast<std::uint32_t>(tokens[index].start)};
}

std::optional<TextOffset> enclosing_list(const std::vector<DatumToken>& tokens, std::size_t from) {
    const DatumToken* selected = nullptr;
    for (const DatumToken& token : tokens) {
        if (token.kind != DatumTokenKind::Open || token.start >= from) {
            continue;
        }
        const bool encloses = !token.pair || from <= tokens[*token.pair].start;
        if (encloses && (selected == nullptr || selected->start < token.start)) {
            selected = &token;
        }
    }
    return selected
               ? std::optional<TextOffset>{TextOffset{static_cast<std::uint32_t>(selected->start)}}
               : std::nullopt;
}

std::optional<std::size_t> enclosing_pair(const std::vector<DatumToken>& tokens, std::size_t from) {
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const DatumToken& token = tokens[index];
        if (token.kind == DatumTokenKind::Open && token.pair && token.start < from &&
            from <= tokens[*token.pair].start) {
            selected = index;
        }
    }
    return selected;
}

std::optional<std::size_t> datum_starting_at(const std::vector<DatumToken>& tokens,
                                             std::size_t index) {
    if (index >= tokens.size() || tokens[index].kind == DatumTokenKind::Close) {
        return std::nullopt;
    }
    return index;
}

std::optional<std::size_t> datum_ending_before(const std::vector<DatumToken>& tokens,
                                               std::size_t index) {
    if (index == 0) {
        return std::nullopt;
    }
    --index;
    if (tokens[index].kind == DatumTokenKind::Close) {
        const std::size_t pair = tokens[index].pair.value_or(tokens.size());
        if (pair == tokens.size()) {
            return std::nullopt;
        }
        index = pair;
    } else if (tokens[index].kind != DatumTokenKind::Atom) {
        return std::nullopt;
    }
    while (index > 0 && tokens[index - 1].kind == DatumTokenKind::Prefix) {
        --index;
    }
    return index;
}

bool token_contains(const Token& token, std::size_t position) {
    return token.range.start.value < position && position < token.range.end.value;
}

bool literal_or_comment_at(const std::vector<Token>& tokens, std::size_t position) {
    return std::ranges::any_of(tokens, [position](const Token& token) {
        return token_contains(token, position) &&
               (token.kind == TokenKind::StringLiteral || token.kind == TokenKind::LineComment ||
                token.kind == TokenKind::BlockComment || token.kind == TokenKind::CharacterLiteral);
    });
}

std::size_t leading_whitespace(std::string_view line) {
    std::size_t size = 0;
    while (size < line.size() && (line[size] == ' ' || line[size] == '\t')) {
        ++size;
    }
    return size;
}

IndentDecision scheme_indent(const DocumentSnapshot& snapshot, std::uint32_t line) {
    const Text& text = snapshot.content();
    if (line >= text.line_count()) {
        throw std::out_of_range("Scheme indentation line is outside the document");
    }
    const TextOffset start = text.line_start(line);
    const std::string all = text.to_string();
    SchemeScan scan = scan_scheme(all);
    const std::string content = snapshot.substring(text.line_content_range(line));
    const std::size_t content_offset = leading_whitespace(content);

    const Token* containing = nullptr;
    for (const Token& token : scan.tokens) {
        if (token.range.start < start && start < token.range.end) {
            containing = &token;
            break;
        }
    }
    if (containing != nullptr && (containing->kind == TokenKind::StringLiteral ||
                                  containing->kind == TokenKind::BlockComment)) {
        return {.target_column = 0,
                .indentation_text = {},
                .role = containing->kind == TokenKind::StringLiteral
                            ? FormatRole::PreservedRawString
                            : FormatRole::PreservedBlockComment,
                .anchor = std::nullopt,
                .preserve = true,
                .trace = {"Scheme literal/comment continuation is preserved"}};
    }

    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < scan.datums.size(); ++index) {
        const DatumToken& token = scan.datums[index];
        if (token.kind != DatumTokenKind::Open || token.start >= start.value) {
            continue;
        }
        if (!token.pair || scan.datums[*token.pair].start >= start.value) {
            selected = index;
        }
    }

    std::uint32_t target = 0;
    FormatRole role = FormatRole::File;
    std::optional<TextOffset> anchor;
    if (selected) {
        const DatumToken& open = scan.datums[*selected];
        const std::uint32_t open_column =
            text.position(TextOffset{static_cast<std::uint32_t>(open.start)}).byte_column;
        target = open_column + 1;
        role = FormatRole::ParenContinuation;
        anchor = TextOffset{static_cast<std::uint32_t>(open.start)};
        if (content_offset < content.size() && is_close(content[content_offset])) {
            target = open_column;
            role = FormatRole::ClosingToken;
        } else if (*selected + 1 < scan.datums.size()) {
            const DatumToken& head = scan.datums[*selected + 1];
            if (head.kind == DatumTokenKind::Atom && head.start < start.value) {
                const std::string_view spelling(all.data() + head.start, head.end - head.start);
                if (is_keyword(spelling)) {
                    target = open_column + 2;
                } else if (*selected + 2 < scan.datums.size()) {
                    const DatumToken& argument = scan.datums[*selected + 2];
                    if (argument.start < start.value &&
                        text.position(TextOffset{static_cast<std::uint32_t>(argument.start)})
                                .line ==
                            text.position(TextOffset{static_cast<std::uint32_t>(open.start)})
                                .line) {
                        target =
                            text.position(TextOffset{static_cast<std::uint32_t>(argument.start)})
                                .byte_column;
                    }
                }
            }
        }
    }
    return {.target_column = static_cast<int>(target),
            .indentation_text = std::string(target, ' '),
            .role = role,
            .anchor = anchor,
            .preserve = false,
            .trace = {"Scheme datum nesting determines indentation"}};
}

class SchemeMechanismSession final : public LanguageMechanismSession {
public:
    const Analysis& analysis(const DocumentSnapshot& snapshot) override {
        if (!analysis_ || analysis_->revision != snapshot.revision()) {
            analysis_ = analyze_scheme(snapshot);
        }
        return *analysis_;
    }

    void apply(const DocumentChange&, const DocumentSnapshot&) override { analysis_.reset(); }

    TypeCharsResult type_chars(Document& document, std::span<const TextOffset> carets,
                               char character, const CppIndentStyle&) override {
        if (carets.empty()) {
            throw std::invalid_argument("typed character requires at least one caret");
        }
        const DocumentSnapshot before = document.snapshot();
        const std::string text = before.content().to_string();
        const SchemeScan scan = scan_scheme(text);
        std::vector<TextOffset> unique(carets.begin(), carets.end());
        std::ranges::sort(unique);
        unique.erase(std::ranges::unique(unique).begin(), unique.end());
        if (unique.back().value > text.size()) {
            throw std::out_of_range("typed character caret is outside the document");
        }

        struct Plan {
            TextOffset at;
            std::string insertion;
            std::uint32_t advance = 0;
        };
        std::vector<Plan> plans;
        plans.reserve(unique.size());
        for (const TextOffset caret : unique) {
            const bool protected_context = literal_or_comment_at(scan.tokens, caret.value);
            const char next = caret.value < text.size() ? text[caret.value] : '\0';
            if (!protected_context && is_open(character)) {
                plans.push_back({caret, std::string{character, matching_close(character)}, 1});
            } else if (next == character &&
                       ((!protected_context && is_close(character)) || character == '"')) {
                plans.push_back({caret, {}, 1});
            } else if (character == '"' && !protected_context) {
                plans.push_back({caret, "\"\"", 1});
            } else {
                plans.push_back({caret, std::string(1, character), 1});
            }
        }

        EditTransaction tx = document.begin_transaction();
        for (auto plan = plans.rbegin(); plan != plans.rend(); ++plan) {
            if (!plan->insertion.empty()) {
                tx.insert(plan->at, plan->insertion);
            }
        }
        CommitResult commit = tx.commit();
        analysis_.reset();

        std::vector<TextOffset> settled;
        settled.reserve(plans.size());
        std::uint64_t delta = 0;
        for (const Plan& plan : plans) {
            const std::uint64_t position = plan.at.value + delta + plan.advance;
            if (position > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("typed character result exceeds the text offset limit");
            }
            settled.push_back(TextOffset{static_cast<std::uint32_t>(position)});
            delta += plan.insertion.size();
        }

        TypeCharsResult result{.carets = {}, .decisions = {}, .change = std::move(commit.change)};
        result.carets.reserve(carets.size());
        result.decisions.resize(carets.size());
        for (const TextOffset caret : carets) {
            const auto found = std::ranges::lower_bound(unique, caret);
            result.carets.push_back(settled[static_cast<std::size_t>(found - unique.begin())]);
        }
        return result;
    }

    EnterResult newline(Document& document, TextOffset caret, const CppIndentStyle&) override {
        EditTransaction tx = document.begin_transaction();
        tx.insert(caret, "\n");
        const DocumentSnapshot speculative = tx.speculative_snapshot();
        const std::uint32_t line = speculative.content().position(TextOffset{caret.value + 1}).line;
        const IndentDecision decision = scheme_indent(speculative, line);
        if (!decision.preserve && !decision.indentation_text.empty()) {
            tx.insert(TextOffset{caret.value + 1}, decision.indentation_text);
        }
        CommitResult commit = tx.commit();
        analysis_.reset();
        return {.handler = "SchemeNewline",
                .decision = decision,
                .caret = TextOffset{caret.value + 1 +
                                    static_cast<std::uint32_t>(decision.indentation_text.size())},
                .change = std::move(commit.change)};
    }

    IndentDecision indent_line(Document& document, std::uint32_t line,
                               const CppIndentStyle&) override {
        const DocumentSnapshot snapshot = document.snapshot();
        IndentDecision decision = scheme_indent(snapshot, line);
        if (decision.preserve) {
            return decision;
        }
        const TextRange range = snapshot.content().line_content_range(line);
        const std::string content = snapshot.substring(range);
        const std::size_t current = leading_whitespace(content);
        if (content.substr(0, current) != decision.indentation_text) {
            EditTransaction tx = document.begin_transaction();
            tx.replace(TextRange{range.start, TextOffset{range.start.value +
                                                         static_cast<std::uint32_t>(current)}},
                       decision.indentation_text);
            (void)tx.commit();
            analysis_.reset();
        }
        return decision;
    }

    IndentDecision explain_indent(const DocumentSnapshot& snapshot, std::uint32_t line,
                                  const CppIndentStyle&) override {
        return scheme_indent(snapshot, line);
    }

    std::optional<TextOffset> move_structurally(const DocumentSnapshot& snapshot, TextOffset from,
                                                StructuralMotion motion) override {
        const std::vector<DatumToken> tokens = scan_scheme(snapshot.content().to_string()).datums;
        switch (motion) {
        case StructuralMotion::ForwardExpression:
            return forward_datum(tokens, from.value);
        case StructuralMotion::BackwardExpression:
            return backward_datum(tokens, from.value);
        case StructuralMotion::UpList:
            return enclosing_list(tokens, from.value);
        }
        return std::nullopt;
    }

    std::expected<StructuralEditResult, std::string>
    edit_structure(Document& document, TextOffset caret, StructuralEdit edit) override {
        const std::string text = document.snapshot().content().to_string();
        const std::vector<DatumToken> tokens = scan_scheme(text).datums;
        const std::optional<std::size_t> open_index = enclosing_pair(tokens, caret.value);
        const std::size_t open_at = open_index.value_or(tokens.size());
        if (open_at == tokens.size()) {
            return std::unexpected("caret is not inside a balanced Scheme list");
        }
        const std::size_t close_index = tokens[open_at].pair.value_or(tokens.size());
        if (close_index == tokens.size()) {
            return std::unexpected("the enclosing Scheme list is incomplete");
        }
        const DatumToken& open = tokens[open_at];
        const DatumToken& close = tokens[close_index];

        const AnchorId caret_anchor = document.create_anchor(caret, AnchorAffinity::AfterInsertion);
        try {
            EditTransaction tx = document.begin_transaction();
            switch (edit) {
            case StructuralEdit::Splice:
                tx.erase(TextRange{TextOffset{static_cast<std::uint32_t>(close.start)},
                                   TextOffset{static_cast<std::uint32_t>(close.end)}});
                tx.erase(TextRange{TextOffset{static_cast<std::uint32_t>(open.start)},
                                   TextOffset{static_cast<std::uint32_t>(open.end)}});
                break;
            case StructuralEdit::ForwardSlurp: {
                const std::optional<std::size_t> next = datum_starting_at(tokens, close_index + 1);
                if (!next) {
                    tx.abort();
                    document.remove_anchor(caret_anchor);
                    return std::unexpected("there is no following datum to slurp");
                }
                const std::optional<std::size_t> end = forward_token(tokens, *next);
                if (!end) {
                    tx.abort();
                    document.remove_anchor(caret_anchor);
                    return std::unexpected("the following datum is incomplete");
                }
                const char delimiter = text[close.start];
                tx.erase(TextRange{TextOffset{static_cast<std::uint32_t>(close.start)},
                                   TextOffset{static_cast<std::uint32_t>(close.end)}});
                tx.insert(TextOffset{static_cast<std::uint32_t>(tokens[*end].end - 1)},
                          std::string_view(&delimiter, 1));
                break;
            }
            case StructuralEdit::ForwardBarf: {
                const std::optional<std::size_t> last = datum_ending_before(tokens, close_index);
                if (!last || *last <= open_at) {
                    tx.abort();
                    document.remove_anchor(caret_anchor);
                    return std::unexpected("the list has no datum to barf");
                }
                const char delimiter = text[close.start];
                std::size_t target = tokens[*last].start;
                while (target > open.end && is_space(text[target - 1])) {
                    --target;
                }
                tx.erase(TextRange{TextOffset{static_cast<std::uint32_t>(close.start)},
                                   TextOffset{static_cast<std::uint32_t>(close.end)}});
                tx.insert(TextOffset{static_cast<std::uint32_t>(target)},
                          std::string_view(&delimiter, 1));
                break;
            }
            case StructuralEdit::BackwardSlurp: {
                const std::optional<std::size_t> previous = datum_ending_before(tokens, open_at);
                if (!previous) {
                    tx.abort();
                    document.remove_anchor(caret_anchor);
                    return std::unexpected("there is no preceding datum to slurp");
                }
                const char delimiter = text[open.start];
                tx.erase(TextRange{TextOffset{static_cast<std::uint32_t>(open.start)},
                                   TextOffset{static_cast<std::uint32_t>(open.end)}});
                tx.insert(TextOffset{static_cast<std::uint32_t>(tokens[*previous].start)},
                          std::string_view(&delimiter, 1));
                break;
            }
            case StructuralEdit::BackwardBarf: {
                const std::optional<std::size_t> first = datum_starting_at(tokens, open_at + 1);
                if (!first || *first >= close_index) {
                    tx.abort();
                    document.remove_anchor(caret_anchor);
                    return std::unexpected("the list has no datum to barf");
                }
                const std::optional<std::size_t> end = forward_token(tokens, *first);
                if (!end || *end >= close_index) {
                    tx.abort();
                    document.remove_anchor(caret_anchor);
                    return std::unexpected("the list has no leading datum to barf");
                }
                const char delimiter = text[open.start];
                std::size_t target = tokens[*end].end;
                while (target < close.start && is_space(text[target])) {
                    ++target;
                }
                tx.erase(TextRange{TextOffset{static_cast<std::uint32_t>(open.start)},
                                   TextOffset{static_cast<std::uint32_t>(open.end)}});
                tx.insert(TextOffset{static_cast<std::uint32_t>(target - 1)},
                          std::string_view(&delimiter, 1));
                break;
            }
            }
            CommitResult commit = tx.commit();
            const TextOffset settled = document.anchor_offset(caret_anchor);
            document.remove_anchor(caret_anchor);
            analysis_.reset();
            return StructuralEditResult{settled, std::move(commit.change)};
        } catch (...) {
            document.remove_anchor(caret_anchor);
            throw;
        }
    }

private:
    std::optional<Analysis> analysis_;
};

std::shared_ptr<const LanguageMechanism> scheme_mechanism() {
    static const std::shared_ptr<const LanguageMechanism> mechanism =
        std::make_shared<const LanguageMechanism>(
            LanguageFacet::Lexing | LanguageFacet::Indentation | LanguageFacet::StructuralMotion |
                LanguageFacet::StructuralEditing | LanguageFacet::Highlighting,
            [] { return std::make_unique<SchemeMechanismSession>(); });
    return mechanism;
}

} // namespace

SchemeMechanismsRegistration ensure_scheme_mechanisms(EditorRuntime& runtime) {
    const std::shared_ptr<const LanguageMechanism> mechanism = scheme_mechanism();
    const auto provider = [&](std::string name, LanguageFacet facet) {
        if (const std::optional<LanguageProviderId> existing =
                runtime.languages().find_provider(name)) {
            const LanguageRegistry::ProviderDefinition& definition =
                runtime.languages().provider(*existing);
            if (definition.facet != facet || definition.mechanism != mechanism) {
                throw std::logic_error("Scheme provider has an incompatible mechanism");
            }
            return *existing;
        }
        return runtime.languages().define_provider(std::move(name), facet, mechanism);
    };
    return {.lexer = provider("cind.scheme.lexer", LanguageFacet::Lexing),
            .indentation = provider("cind.scheme.indentation", LanguageFacet::Indentation),
            .structural_motion =
                provider("cind.scheme.structural-motion", LanguageFacet::StructuralMotion),
            .structural_editing =
                provider("cind.scheme.structural-editing", LanguageFacet::StructuralEditing),
            .highlighting = provider("cind.scheme.highlighting", LanguageFacet::Highlighting)};
}

} // namespace cind
