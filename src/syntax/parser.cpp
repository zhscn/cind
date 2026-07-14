#include "cpp_lexer/lexer.hpp"
#include "syntax/syntax_tree.hpp"

#include <vector>

namespace cind {

namespace {

// design.md §8.4: tokens that resynchronize the parser after opaque or
// broken constructs. Seeing one of these mid-declaration ends the current
// node instead of swallowing distant structure.
// FOO_BAR(...) with nothing after it on the line: a macro invocation used
// as a declaration (LLVM_YAML_IS_SEQUENCE_VECTOR, TEST_F without a body on
// the same construct). All-caps naming is the cc-mode/CLion heuristic.
bool is_macro_name(std::string_view s) {
    if (s.size() < 3) {
        return false;
    }
    for (char c : s) {
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

bool is_sync_introducer(TokenKind k) {
    switch (k) {
    case TokenKind::NamespaceKw:
    case TokenKind::ClassKw:
    case TokenKind::StructKw:
    case TokenKind::EnumKw:
    case TokenKind::UnionKw:
    case TokenKind::SwitchKw:
    case TokenKind::IfKw:
    case TokenKind::ElseKw:
    case TokenKind::ForKw:
    case TokenKind::WhileKw:
    case TokenKind::DoKw:
    case TokenKind::CaseKw:
    case TokenKind::DefaultKw:
    case TokenKind::PublicKw:
    case TokenKind::ProtectedKw:
    case TokenKind::PrivateKw:
    case TokenKind::PreprocessorHash: return true;
    default: return false;
    }
}

} // namespace

// In cpp-only scope; declared a friend of SyntaxTree.
class Parser {
public:
    Parser(std::string_view text, LexOutput lexed) : text_(text) {
        tree_.tokens_ = std::move(lexed.tokens);
    }

    SyntaxTree run() {
        nodes().push_back(SyntaxNode{SyntaxKind::TranslationUnit, 0, 0, kInvalidNode, {}, false,
                                     TokenKind::EndOfFile});
        stack_.push_back(0);
        while (!at_eof()) {
            guarded([&] { parse_declaration_or_statement(); });
        }
        pos_ = static_cast<std::uint32_t>(tree_.tokens_.size()); // include trailing trivia + EOF
        nodes()[0].end_token = pos_;
        stack_.pop_back();
        return std::move(tree_);
    }

private:
    std::vector<SyntaxNode>& nodes() { return tree_.nodes_; }
    const std::vector<Token>& tokens() const { return tree_.tokens_; }

    // -- cursor ------------------------------------------------------------

    void skip_trivia() {
        while (is_trivia(tokens()[pos_].kind)) {
            ++pos_;
        }
    }
    TokenKind kind() {
        skip_trivia();
        return tokens()[pos_].kind;
    }
    bool at(TokenKind k) { return kind() == k; }
    bool at_eof() { return at(TokenKind::EndOfFile); }
    void advance() {
        skip_trivia();
        if (tokens()[pos_].kind != TokenKind::EndOfFile) {
            ++pos_;
        }
    }
    std::string_view token_text() {
        skip_trivia();
        const Token& t = tokens()[pos_];
        return text_.substr(t.range.start.value, t.range.length());
    }
    // Declarator-suffix tokens between ')' (or ']') and '{' that keep a
    // function/lambda body possible: `) const {`, `) mutable -> T & {`.
    bool keeps_body_pending() {
        const TokenKind k = kind();
        if (k == TokenKind::Identifier || k == TokenKind::ColonColon ||
            k == TokenKind::Arrow) {
            return true;
        }
        if (k != TokenKind::Punctuator) {
            return false;
        }
        const std::string_view s = token_text();
        return s == "&" || s == "&&" || s == "*";
    }

    bool newline_before_next_token() const {
        for (std::uint32_t i = pos_; i < tokens().size(); ++i) {
            const TokenKind k = tokens()[i].kind;
            if (k == TokenKind::Newline || k == TokenKind::EndOfFile) {
                return true;
            }
            if (!is_trivia(k)) {
                return false;
            }
        }
        return true;
    }

    // -- tree building -----------------------------------------------------

    SyntaxNodeId open(SyntaxKind kind) {
        skip_trivia(); // leading trivia belongs to the parent
        const auto id = static_cast<SyntaxNodeId>(nodes().size());
        nodes().push_back(
            SyntaxNode{kind, pos_, pos_, stack_.back(), {}, false, TokenKind::EndOfFile});
        nodes()[stack_.back()].children.push_back(id);
        stack_.push_back(id);
        return id;
    }

    void close(SyntaxNodeId id) {
        // Trailing trivia belongs to the parent. Lookahead (kind()/at())
        // skips trivia in place, so trim it back off — but never past the
        // last child (a MissingToken may legitimately sit at pos_).
        SyntaxNode& n = nodes()[id];
        std::uint32_t floor = n.children.empty() ? n.first_token : nodes()[n.children.back()].end_token;
        std::uint32_t end = pos_;
        while (end > floor && is_trivia(tokens()[end - 1].kind)) {
            --end;
        }
        n.end_token = end;
        stack_.pop_back();
    }

    void mark_incomplete(SyntaxNodeId id) { nodes()[id].incomplete = true; }

    // Zero-length node recording an expected-but-absent token (Roslyn-style
    // full fidelity: the parser never fakes input). Marks the innermost open
    // node incomplete.
    void add_missing(TokenKind expected) {
        skip_trivia();
        const auto id = static_cast<SyntaxNodeId>(nodes().size());
        nodes().push_back(
            SyntaxNode{SyntaxKind::MissingToken, pos_, pos_, stack_.back(), {}, true, expected});
        nodes()[stack_.back()].children.push_back(id);
        nodes()[stack_.back()].incomplete = true;
    }

    void expect_or_missing(TokenKind k) {
        if (at(k)) {
            advance();
        } else {
            add_missing(k);
        }
    }

    // Progress guarantee: a production that consumed nothing forfeits one
    // token into an Error node, so no loop can spin on the same position.
    template <typename F> void guarded(F&& production) {
        const std::uint32_t before = pos_;
        production();
        if (pos_ == before) {
            const SyntaxNodeId e = open(SyntaxKind::Error);
            advance();
            close(e);
        }
    }

    // -- productions ---------------------------------------------------------

    void parse_declaration_or_statement() {
        switch (kind()) {
        case TokenKind::PreprocessorHash: parse_pp_directive(); return;
        case TokenKind::NamespaceKw: parse_namespace(); return;
        case TokenKind::ClassKw:
        case TokenKind::StructKw:
        case TokenKind::UnionKw:
        case TokenKind::EnumKw: parse_class(); return;
        case TokenKind::PublicKw:
        case TokenKind::ProtectedKw:
        case TokenKind::PrivateKw: parse_access_label(); return;
        case TokenKind::TemplateKw: parse_template_prefix(); return;
        case TokenKind::SwitchKw: parse_switch(); return;
        case TokenKind::IfKw: parse_if(); return;
        case TokenKind::ForKw:
        case TokenKind::WhileKw: parse_loop(); return;
        case TokenKind::DoKw: parse_do(); return;
        case TokenKind::CaseKw:
        case TokenKind::DefaultKw: parse_case_section(); return;
        case TokenKind::LBrace: parse_compound_statement(); return;
        case TokenKind::Semicolon: {
            const SyntaxNodeId n = open(SyntaxKind::OpaqueDeclaration);
            advance();
            close(n);
            return;
        }
        case TokenKind::RBrace:
        case TokenKind::EndOfFile: return; // the caller owns these
        default: parse_generic(); return;
        }
    }

    void parse_pp_directive() {
        const SyntaxNodeId d = open(SyntaxKind::PreprocessorDirective);
        // Raw consumption: the directive owns its trivia up to and including
        // the terminating (still-flagged) newline.
        while (pos_ + 1 < tokens().size() &&
               has_flag(tokens()[pos_].flags, LexicalFlags::PreprocessorLine)) {
            ++pos_;
        }
        close(d);
    }

    void parse_namespace() {
        const SyntaxNodeId n = open(SyntaxKind::NamespaceDecl);
        advance(); // namespace
        while (at(TokenKind::Identifier) || at(TokenKind::ColonColon)) {
            advance();
        }
        if (at(TokenKind::Equals)) { // namespace alias
            while (!at_eof() && !at(TokenKind::Semicolon) && !at(TokenKind::RBrace) &&
                   !is_sync_introducer(kind())) {
                advance();
            }
            if (at(TokenKind::Semicolon)) {
                advance();
            }
            close(n);
            return;
        }
        if (at(TokenKind::LBrace)) {
            const SyntaxNodeId body = open(SyntaxKind::NamespaceBody);
            advance();
            while (!at_eof() && !at(TokenKind::RBrace)) {
                guarded([&] { parse_declaration_or_statement(); });
            }
            expect_or_missing(TokenKind::RBrace);
            close(body);
        } else {
            add_missing(TokenKind::LBrace);
        }
        close(n);
    }

    void parse_class() {
        const SyntaxNodeId n = open(SyntaxKind::ClassDecl);
        advance(); // class/struct/union/enum
        while (!at_eof() && !at(TokenKind::LBrace) && !at(TokenKind::Semicolon) &&
               !at(TokenKind::RBrace) && kind() != TokenKind::NamespaceKw) {
            if (at(TokenKind::LParen)) {
                parse_paren_group();
            } else if (at(TokenKind::Less)) {
                if (!parse_template_args()) {
                    advance();
                }
            } else {
                advance(); // head: name, base clause, attributes
            }
        }
        if (at(TokenKind::LBrace)) {
            const SyntaxNodeId body = open(SyntaxKind::ClassBody);
            advance();
            while (!at_eof() && !at(TokenKind::RBrace)) {
                guarded([&] { parse_declaration_or_statement(); });
            }
            expect_or_missing(TokenKind::RBrace);
            close(body);
            if (at(TokenKind::Semicolon)) {
                advance();
            }
        } else if (at(TokenKind::Semicolon)) {
            advance();
        } else {
            mark_incomplete(n); // "class Foo" mid-typing
        }
        close(n);
    }

    void parse_access_label() {
        const SyntaxNodeId n = open(SyntaxKind::AccessSpecifierLabel);
        advance(); // public/protected/private
        while (at(TokenKind::Identifier)) {
            advance(); // e.g. Qt "public slots"
        }
        expect_or_missing(TokenKind::Colon);
        close(n);
    }

    void parse_template_prefix() {
        const SyntaxNodeId n = open(SyntaxKind::OpaqueDeclaration);
        advance(); // template
        if (at(TokenKind::Less) && !parse_template_args()) {
            advance();
        }
        close(n); // the templated entity parses as the next sibling
    }

    void parse_if() {
        const SyntaxNodeId n = open(SyntaxKind::IfStatement);
        advance(); // if
        while (at(TokenKind::Identifier)) {
            advance(); // constexpr / consteval
        }
        if (at(TokenKind::LParen)) {
            parse_paren_group();
        } else {
            add_missing(TokenKind::LParen);
        }
        parse_embedded_statement(n);
        if (at(TokenKind::ElseKw)) {
            const SyntaxNodeId e = open(SyntaxKind::ElseClause);
            advance();
            parse_embedded_statement(e);
            close(e);
        }
        close(n);
    }

    void parse_loop() {
        const SyntaxNodeId n =
            open(at(TokenKind::ForKw) ? SyntaxKind::ForStatement : SyntaxKind::WhileStatement);
        advance();
        if (at(TokenKind::LParen)) {
            parse_paren_group();
        } else {
            add_missing(TokenKind::LParen);
        }
        parse_embedded_statement(n);
        close(n);
    }

    void parse_do() {
        const SyntaxNodeId n = open(SyntaxKind::DoStatement);
        advance(); // do
        if (!at(TokenKind::WhileKw)) {
            parse_embedded_statement(n);
        }
        if (at(TokenKind::WhileKw)) {
            advance();
            if (at(TokenKind::LParen)) {
                parse_paren_group();
            } else {
                add_missing(TokenKind::LParen);
            }
            expect_or_missing(TokenKind::Semicolon);
        } else {
            mark_incomplete(n);
        }
        close(n);
    }

    // A control statement's body: single statement or compound. Absent body
    // (closing brace, else, EOF next) marks the owner incomplete — this is
    // what "Enter after if (x)" keys the extra indent on.
    void parse_embedded_statement(SyntaxNodeId owner) {
        const TokenKind k = kind();
        if (k == TokenKind::RBrace || k == TokenKind::EndOfFile || k == TokenKind::ElseKw ||
            k == TokenKind::CaseKw || k == TokenKind::DefaultKw) {
            mark_incomplete(owner);
            return;
        }
        guarded([&] { parse_declaration_or_statement(); });
    }

    void parse_switch() {
        const SyntaxNodeId n = open(SyntaxKind::SwitchStatement);
        advance(); // switch
        if (at(TokenKind::LParen)) {
            parse_paren_group();
        } else {
            add_missing(TokenKind::LParen);
        }
        if (at(TokenKind::LBrace)) {
            const SyntaxNodeId body = open(SyntaxKind::CompoundStatement);
            advance();
            while (!at_eof() && !at(TokenKind::RBrace)) {
                guarded([&] { parse_declaration_or_statement(); });
            }
            expect_or_missing(TokenKind::RBrace);
            close(body);
        } else {
            mark_incomplete(n);
        }
        close(n);
    }

    void parse_case_section() {
        const SyntaxNodeId section = open(SyntaxKind::CaseSection);
        {
            const SyntaxNodeId label = open(SyntaxKind::CaseLabel);
            advance(); // case/default
            while (!at_eof() && !at(TokenKind::Colon) && !at(TokenKind::Semicolon) &&
                   !at(TokenKind::LBrace) && !at(TokenKind::RBrace)) {
                if (at(TokenKind::LParen)) {
                    parse_paren_group();
                } else {
                    advance();
                }
            }
            expect_or_missing(TokenKind::Colon);
            close(label);
        }
        while (!at_eof() && !at(TokenKind::RBrace) && !at(TokenKind::CaseKw) &&
               !at(TokenKind::DefaultKw)) {
            guarded([&] { parse_declaration_or_statement(); });
        }
        close(section);
    }

    void parse_compound_statement() {
        const SyntaxNodeId c = open(SyntaxKind::CompoundStatement);
        advance(); // {
        while (!at_eof() && !at(TokenKind::RBrace)) {
            guarded([&] { parse_declaration_or_statement(); });
        }
        expect_or_missing(TokenKind::RBrace);
        close(c);
    }

    void parse_paren_group() {
        const SyntaxNodeId g = open(SyntaxKind::ParenGroup);
        advance(); // (
        bool body_pending = false; // ')'/']' + declarator suffixes seen
        while (!at_eof()) {
            const TokenKind k = kind();
            if (k == TokenKind::RParen) {
                advance();
                close(g);
                return;
            }
            if (k == TokenKind::LParen) {
                parse_paren_group();
                body_pending = true;
                continue;
            }
            if (k == TokenKind::LBracket) {
                parse_bracket_group();
                body_pending = true;
                continue;
            }
            if (k == TokenKind::LBrace) {
                // ')' or ']' (plus suffixes) before '{' inside an
                // expression: a lambda body.
                if (body_pending) {
                    parse_compound_statement();
                } else {
                    parse_brace_group();
                }
                body_pending = false;
                continue;
            }
            // Unclosed paren: bail at structure that clearly is not an
            // argument, so damage stays bounded (design.md §7.4b, §8.4).
            if (k == TokenKind::RBrace || k == TokenKind::NamespaceKw ||
                k == TokenKind::ClassKw || k == TokenKind::StructKw) {
                break;
            }
            body_pending = body_pending && keeps_body_pending();
            advance(); // ';' stays allowed: for (;;)
        }
        add_missing(TokenKind::RParen);
        close(g);
    }

    void parse_bracket_group() {
        const SyntaxNodeId g = open(SyntaxKind::BracketGroup);
        advance(); // [
        while (!at_eof()) {
            const TokenKind k = kind();
            if (k == TokenKind::RBracket) {
                advance();
                close(g);
                return;
            }
            if (k == TokenKind::LParen) {
                parse_paren_group();
                continue;
            }
            if (k == TokenKind::LBracket) {
                parse_bracket_group();
                continue;
            }
            if (k == TokenKind::Semicolon || k == TokenKind::LBrace || k == TokenKind::RBrace ||
                is_sync_introducer(k)) {
                break;
            }
            advance();
        }
        add_missing(TokenKind::RBracket);
        close(g);
    }

    void parse_brace_group() {
        const SyntaxNodeId g = open(SyntaxKind::BraceGroup);
        advance(); // {
        bool body_pending = false; // ')'/']' + declarator suffixes seen
        while (!at_eof()) {
            const TokenKind k = kind();
            if (k == TokenKind::RBrace) {
                advance();
                close(g);
                return;
            }
            if (k == TokenKind::LParen) {
                parse_paren_group();
                body_pending = true;
                continue;
            }
            if (k == TokenKind::LBracket) {
                parse_bracket_group();
                body_pending = true;
                continue;
            }
            if (k == TokenKind::LBrace) {
                if (body_pending) {
                    parse_compound_statement();
                } else {
                    parse_brace_group();
                }
                body_pending = false;
                continue;
            }
            // 'return' can only reach a brace-init directly through a
            // misparse; bail so the damage stays bounded.
            if (k == TokenKind::Semicolon || k == TokenKind::ReturnKw ||
                is_sync_introducer(k)) {
                break;
            }
            body_pending = body_pending && keeps_body_pending();
            advance();
        }
        add_missing(TokenKind::RBrace);
        close(g);
    }

    // Template-angle heuristic (design.md §7.4a): only pair '<...>' in
    // declarative positions; give up on statement structure or imbalance and
    // let '<' stay an ordinary operator. Never produces an error node.
    std::uint32_t match_angles(std::uint32_t from) {
        int angle = 0;
        int paren = 0;
        int bracket = 0;
        int significant = 0;
        for (std::uint32_t i = from; i < tokens().size(); ++i) {
            const Token& t = tokens()[i];
            if (is_trivia(t.kind)) {
                continue;
            }
            if (++significant > 200) {
                return 0;
            }
            switch (t.kind) {
            case TokenKind::Less: ++angle; break;
            case TokenKind::Greater:
                if (--angle == 0) {
                    return i + 1;
                }
                break;
            case TokenKind::Punctuator:
                if (text_.substr(t.range.start.value, t.range.length()) == ">>") {
                    angle -= 2;
                    if (angle <= 0) {
                        return i + 1;
                    }
                }
                break;
            case TokenKind::LParen: ++paren; break;
            case TokenKind::RParen:
                if (paren == 0) {
                    return 0;
                }
                --paren;
                break;
            case TokenKind::LBracket: ++bracket; break;
            case TokenKind::RBracket:
                if (bracket == 0) {
                    return 0;
                }
                --bracket;
                break;
            case TokenKind::Semicolon:
            case TokenKind::LBrace:
            case TokenKind::RBrace:
            case TokenKind::EndOfFile: return 0;
            default: break;
            }
        }
        return 0;
    }

    bool parse_template_args() {
        skip_trivia();
        const std::uint32_t end = match_angles(pos_);
        if (end == 0) {
            return false;
        }
        const SyntaxNodeId a = open(SyntaxKind::TemplateArgumentList);
        pos_ = end; // contents stay opaque
        close(a);
        return true;
    }

    void parse_ctor_initializer_list() {
        const SyntaxNodeId list = open(SyntaxKind::CtorInitializerList);
        advance(); // ':'
        bool expect_item = true;
        while (!at_eof()) {
            const TokenKind k = kind();
            if (k == TokenKind::LBrace || k == TokenKind::RBrace || k == TokenKind::Semicolon ||
                is_sync_introducer(k)) {
                break;
            }
            if (k == TokenKind::Comma) {
                advance();
                expect_item = true;
                continue;
            }
            const std::uint32_t before = pos_;
            const SyntaxNodeId item = open(SyntaxKind::CtorInitializer);
            bool named = false;
            while (at(TokenKind::Identifier) || at(TokenKind::ColonColon)) {
                advance();
                named = true;
            }
            if (at(TokenKind::Less) && named) {
                parse_template_args();
            }
            if (at(TokenKind::LParen)) {
                parse_paren_group();
            } else if (at(TokenKind::LBrace) && named) {
                parse_brace_group(); // member brace-init: b_{0}
            } else {
                mark_incomplete(item); // e.g. ": a_" or ": a_(1), b_" mid-typing
            }
            if (pos_ == before) {
                advance(); // progress guarantee
            }
            expect_item = false;
            if (nodes()[item].incomplete) {
                mark_incomplete(list);
            }
            close(item);
        }
        if (expect_item) {
            mark_incomplete(list); // ":" or trailing "," with nothing after
        }
        close(list);
    }

    // Fallback for everything else: one opaque declaration/statement,
    // upgraded to FunctionDefinition when a body or ctor-initializer shows
    // up. Expressions stay opaque token runs.
    void parse_generic() {
        const SyntaxNodeId n = open(SyntaxKind::OpaqueDeclaration);
        TokenKind prev = TokenKind::EndOfFile; // sentinel: nothing consumed yet
        bool saw_question = false;
        bool terminated = false;
        // True after a ')' with only declarator suffixes since — const,
        // noexcept(...), override/final, ref-qualifiers, a trailing return
        // type. A '{' then still opens a function body ("`) const {`").
        bool header_pending = false;
        // True while the node consists of exactly one all-caps identifier;
        // `MACRO(...)` followed by a line break then ends the declaration.
        bool macro_ident_only = false;

        while (!at_eof()) {
            const TokenKind k = kind();
            if (k == TokenKind::Semicolon) {
                advance();
                terminated = true;
                break;
            }
            if (k == TokenKind::RBrace) {
                mark_incomplete(n);
                break;
            }
            if (is_sync_introducer(k)) {
                if (k == TokenKind::DefaultKw && prev == TokenKind::Equals) {
                    advance(); // `= default;` — not a case label
                    prev = k;
                    continue;
                }
                mark_incomplete(n);
                break;
            }
            if (k == TokenKind::LParen) {
                parse_paren_group();
                if (macro_ident_only && newline_before_next_token()) {
                    terminated = true;
                    break;
                }
                macro_ident_only = false;
                prev = TokenKind::RParen;
                header_pending = true;
                continue;
            }
            if (k == TokenKind::LBracket) {
                parse_bracket_group();
                macro_ident_only = false;
                prev = TokenKind::RBracket;
                continue;
            }
            if (k == TokenKind::LBrace) {
                if (header_pending) {
                    nodes()[n].kind = SyntaxKind::FunctionDefinition;
                    parse_compound_statement();
                    terminated = true;
                    break; // no ';' required after a body
                }
                parse_brace_group();
                prev = TokenKind::RBrace;
                continue;
            }
            if (k == TokenKind::Colon && header_pending && !saw_question) {
                nodes()[n].kind = SyntaxKind::FunctionDefinition;
                parse_ctor_initializer_list();
                if (at(TokenKind::LBrace)) {
                    parse_compound_statement();
                } else {
                    add_missing(TokenKind::LBrace);
                }
                terminated = true;
                break;
            }
            if (k == TokenKind::Less &&
                (prev == TokenKind::Identifier || prev == TokenKind::TemplateKw)) {
                if (parse_template_args()) {
                    prev = TokenKind::Greater;
                    continue;
                }
            }
            if (k == TokenKind::Punctuator && token_text() == "?") {
                saw_question = true;
            }
            if (header_pending && k != TokenKind::Identifier && k != TokenKind::ColonColon &&
                k != TokenKind::Arrow) {
                const std::string_view s = token_text();
                if (k != TokenKind::Punctuator || (s != "&" && s != "&&" && s != "*")) {
                    header_pending = false;
                }
            }
            macro_ident_only = prev == TokenKind::EndOfFile && k == TokenKind::Identifier &&
                               is_macro_name(token_text());
            advance();
            prev = k;
        }
        if (!terminated) {
            mark_incomplete(n); // no ';' (or body) yet — mid-typing state
        }
        close(n);
    }

    std::string_view text_;
    std::uint32_t pos_ = 0;
    SyntaxTree tree_;
    std::vector<SyntaxNodeId> stack_;
};

SyntaxTree parse(std::string_view text) { return Parser(text, lex(text)).run(); }

} // namespace cind
