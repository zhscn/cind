#include "cpp_lexer/lexer.hpp"
#include "document/char_source.hpp"
#include "document/text.hpp"
#include "syntax/pp_conditional.hpp"
#include "syntax/syntax_tree.hpp"

#include <algorithm>
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

// Declared a friend of SyntaxTree. Source is a CharSource (char_source.hpp);
// token text is only extracted at a few cold classification sites.
template <typename Source>
class Parser {
public:
    Parser(const Source& source, LexOutput lexed) : src_(source) {
        tree_.tokens_ = std::move(lexed.tokens);
    }

    // Sandbox mode for incremental block reparse: tokens are borrowed, not
    // owned, and parsing replays one container's item loop.
    Parser(const Source& source, const std::vector<Token>& tokens)
        : src_(source), tokens_view_(&tokens) {}

    // Replays the guarded(parse_declaration_or_statement) loop of a
    // `container` node from `start`, stopping as soon as the position (after
    // trivia) lands on one of `boundaries` (sorted, current-token-stream
    // coordinates) — the old parse is provably identical from there on. The
    // sandbox root (node 0) stands in for the container; its direct children
    // are the reparsed items. aligned_at stays kNoBoundary when the loop ran
    // past every boundary or hit a loop-exit token that is not a boundary —
    // the caller escalates to the enclosing container.
    static constexpr std::uint32_t kNoBoundary = 0xFFFFFFFFu;
    struct SandboxResult {
        std::vector<SyntaxNode> nodes;
        std::uint32_t aligned_at = kNoBoundary;
    };
    SandboxResult run_items(std::uint32_t start, SyntaxKind container, bool enum_body,
                            std::span<const std::uint32_t> boundaries) {
        nodes().push_back(SyntaxNode{container, start, start, kInvalidNode, {}, false, false,
                                     TokenKind::EndOfFile});
        stack_.push_back(0);
        pos_ = start;
        enum_body_ = enum_body;
        SandboxResult result;
        while (true) {
            skip_trivia();
            if (std::binary_search(boundaries.begin(), boundaries.end(), pos_)) {
                result.aligned_at = pos_;
                break;
            }
            if (pos_ > boundaries.back()) {
                break; // ran past every boundary: escalate
            }
            const TokenKind k = tokens()[pos_].kind;
            if (k == TokenKind::EndOfFile) {
                break; // a legal EOF stop would have matched a boundary
            }
            if (container != SyntaxKind::TranslationUnit && k == TokenKind::RBrace) {
                break; // container loop exit that is not where the old one was
            }
            if (container == SyntaxKind::CaseSection &&
                (k == TokenKind::CaseKw || k == TokenKind::DefaultKw)) {
                break;
            }
            const PPItem step = pp_open_item();
            if (step == PPItem::StopLoop) {
                break;
            }
            if (step == PPItem::Consumed) {
                continue;
            }
            guarded([&] { parse_declaration_or_statement(); });
            if (pp_escalate_) {
                break;
            }
            if (unwinding()) {
                if (static_cast<std::uint32_t>(stack_.size()) > unwind_target_) {
                    break;
                }
                unwind_target_ = kNoUnwind;
            }
        }
        if (pp_escalate_) {
            result.aligned_at = kNoBoundary; // a conditional opened before our span
            return result;
        }
        result.nodes = std::move(tree_.nodes_);
        return result;
    }

    SyntaxTree run() {
        nodes().push_back(SyntaxNode{SyntaxKind::TranslationUnit, 0, 0, kInvalidNode, {}, false,
                                     false, TokenKind::EndOfFile});
        stack_.push_back(0);
        run_container_items([&] { return at_eof(); });
        pos_ = static_cast<std::uint32_t>(tree_.tokens_.size()); // include trailing trivia + EOF
        nodes()[0].end_token = pos_;
        stack_.pop_back();
        tree_.green_root_ = green_from_flat(tree_);
        return std::move(tree_);
    }

private:
    std::vector<SyntaxNode>& nodes() { return tree_.nodes_; }
    const std::vector<Token>& tokens() const {
        return tokens_view_ != nullptr ? *tokens_view_ : tree_.tokens_;
    }

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
        src_.extract(t.range.start.value, t.range.length(), token_text_buffer_);
        return token_text_buffer_;
    }
    // Declarator-suffix tokens between ')' (or ']') and '{' that keep a
    // function/lambda body possible: `) const {`, `) mutable -> T & {`.
    bool keeps_body_pending() {
        const TokenKind k = kind();
        return k == TokenKind::Identifier || k == TokenKind::ColonColon ||
               k == TokenKind::Arrow || k == TokenKind::Amp || k == TokenKind::AmpAmp ||
               k == TokenKind::Star;
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
            SyntaxNode{kind, pos_, pos_, stack_.back(), {}, false, false, TokenKind::EndOfFile});
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
        nodes().push_back(SyntaxNode{SyntaxKind::MissingToken, pos_, pos_, stack_.back(), {},
                                     true, false, expected});
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

    // The directive owns everything up to and including the terminating
    // (still-flagged) newline. Bracket pairs inside the body become group
    // nodes so multi-line macro bodies get normal continuation/alignment;
    // group parsing never leaves the directive, so an unbalanced macro
    // (`#define LPAREN (`) stays bounded.
    bool pp_line_continues() const {
        return pos_ + 1 < tokens().size() &&
               has_flag(tokens()[pos_].flags, LexicalFlags::PreprocessorLine);
    }

    void parse_pp_group(TokenKind closer, SyntaxKind kind) {
        const SyntaxNodeId g = open(kind);
        ++pos_; // opener
        while (pp_line_continues()) {
            const TokenKind k = tokens()[pos_].kind;
            if (is_trivia(k)) {
                ++pos_;
                continue;
            }
            if (k == closer) {
                ++pos_;
                close(g);
                return;
            }
            if (!parse_pp_body_token(k)) {
                ++pos_;
            }
        }
        add_missing(closer);
        close(g);
    }

    // Returns true if the token opened a nested group (already consumed).
    bool parse_pp_body_token(TokenKind k) {
        switch (k) {
        case TokenKind::LParen: parse_pp_group(TokenKind::RParen, SyntaxKind::ParenGroup); return true;
        case TokenKind::LBracket:
            parse_pp_group(TokenKind::RBracket, SyntaxKind::BracketGroup);
            return true;
        case TokenKind::LBrace: parse_pp_group(TokenKind::RBrace, SyntaxKind::BraceGroup); return true;
        default: return false;
        }
    }

    void parse_pp_directive() {
        const SyntaxNodeId d = open(SyntaxKind::PreprocessorDirective);
        while (pp_line_continues()) {
            const TokenKind k = tokens()[pos_].kind;
            if (is_trivia(k) || !parse_pp_body_token(k)) {
                ++pos_;
            }
        }
        close(d);
    }

    // Classify the preprocessor directive whose '#' is at `hash`.
    PPCat pp_category_at(std::uint32_t hash) {
        std::uint32_t i = hash + 1;
        while (i < tokens().size() && is_trivia(tokens()[i].kind)) {
            ++i;
        }
        if (i >= tokens().size()) {
            return PPCat::Other;
        }
        const TokenKind k = tokens()[i].kind;
        if (k != TokenKind::Identifier) {
            return pp_classify(k, {});
        }
        const Token& t = tokens()[i];
        src_.extract(t.range.start.value, t.range.length(), token_text_buffer_);
        return pp_classify(k, token_text_buffer_);
    }

    enum class PPItem : std::uint8_t {
        Dispatch,  // no conditional directive here: parse the next item normally
        Consumed,  // a conditional directive was consumed; re-run the loop
        StopLoop,  // #else/#elif belongs to an outer container: unwind
    };

    // Called at the top of a container item loop. Handles a leading
    // preprocessor conditional: #if pushes a frame, #else/#elif at the owning
    // level is consumed (branches parse as siblings), #else/#elif belonging to
    // an outer container requests an unwind, #endif pops the frame. #define
    // and friends fall through to normal flat parsing.
    PPItem pp_open_item() {
        skip_trivia();
        if (tokens()[pos_].kind != TokenKind::PreprocessorHash) {
            return PPItem::Dispatch;
        }
        const std::uint32_t depth = static_cast<std::uint32_t>(stack_.size());
        switch (pp_category_at(pos_)) {
        case PPCat::Open:
            parse_pp_directive();
            pp_frames_.push_back({depth});
            return PPItem::Consumed;
        case PPCat::Alt:
            if (pp_frames_.empty()) {
                // The matching #if is not in view. In a full parse this is a
                // genuinely unmatched #else — stay flat, bounded. In a sandbox
                // reparse it means the conditional opened before our span, so
                // we cannot know whether to unwind: escalate to a container
                // that spans the whole conditional.
                if (tokens_view_ != nullptr) {
                    pp_escalate_ = true;
                    return PPItem::StopLoop;
                }
                return PPItem::Dispatch;
            }
            if (pp_frames_.back().owner_depth < depth) {
                unwind_target_ = pp_frames_.back().owner_depth;
                return PPItem::StopLoop;
            }
            parse_pp_directive(); // this loop owns the conditional: consume, keep frame
            return PPItem::Consumed;
        case PPCat::Close:
            if (!pp_frames_.empty()) {
                pp_frames_.pop_back();
            }
            parse_pp_directive();
            return PPItem::Consumed;
        case PPCat::Other:
            return PPItem::Dispatch;
        }
        return PPItem::Dispatch;
    }

    // The shared item loop for every statement/declaration container. `stop`
    // reports the container's own terminators (EOF, '}', case labels). Handles
    // preprocessor conditionals via pp_open_item and the cooperative unwind:
    // when a deeper container requested a restore, break out until the owning
    // loop (stack depth == unwind_target_) resumes.
    template <typename StopFn> void run_container_items(StopFn stop) {
        while (!stop()) {
            if (pp_escalate_) {
                break;
            }
            const PPItem step = pp_open_item();
            if (step == PPItem::StopLoop) {
                break;
            }
            if (step == PPItem::Consumed) {
                continue;
            }
            guarded([&] { parse_declaration_or_statement(); });
            if (pp_escalate_) {
                break;
            }
            if (unwinding()) {
                if (static_cast<std::uint32_t>(stack_.size()) > unwind_target_) {
                    break;
                }
                unwind_target_ = kNoUnwind; // this loop owns the restore: resume
            }
        }
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
            run_container_items([&] { return at_eof() || at(TokenKind::RBrace); });
            expect_or_missing(TokenKind::RBrace);
            close(body);
        } else {
            add_missing(TokenKind::LBrace);
        }
        close(n);
    }

    void parse_class() {
        const SyntaxNodeId n = open(SyntaxKind::ClassDecl);
        const bool is_enum = at(TokenKind::EnumKw);
        advance(); // class/struct/union/enum
        bool saw_equals = false;
        while (!at_eof() && !at(TokenKind::LBrace) && !at(TokenKind::Semicolon) &&
               !at(TokenKind::RBrace) && kind() != TokenKind::NamespaceKw) {
            if (at(TokenKind::LParen)) {
                parse_paren_group();
            } else if (at(TokenKind::Less)) {
                if (!parse_template_args()) {
                    advance();
                }
            } else {
                saw_equals = saw_equals || at(TokenKind::Equals);
                advance(); // head: name, base clause, attributes
            }
        }
        if (saw_equals && at(TokenKind::LBrace)) {
            // `struct f_cnvrt Convert = { ... };` — the keyword introduces the
            // variable's type, and a brace after '=' is always an initializer
            // list, never a class body (clang-format calculateBraceTypes).
            nodes()[n].kind = SyntaxKind::OpaqueDeclaration;
            parse_brace_group();
            if (at(TokenKind::Semicolon)) {
                advance();
            }
            close(n);
            return;
        }
        if (at(TokenKind::LBrace)) {
            const SyntaxNodeId body = open(SyntaxKind::ClassBody);
            advance();
            const bool saved = enum_body_;
            enum_body_ = is_enum;
            run_container_items([&] { return at_eof() || at(TokenKind::RBrace); });
            enum_body_ = saved;
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
            run_container_items([&] { return at_eof() || at(TokenKind::RBrace); });
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
        run_container_items([&] {
            return at_eof() || at(TokenKind::RBrace) || at(TokenKind::CaseKw) ||
                   at(TokenKind::DefaultKw);
        });
        close(section);
    }

    void parse_compound_statement() {
        const SyntaxNodeId c = open(SyntaxKind::CompoundStatement);
        advance(); // {
        run_container_items([&] { return at_eof() || at(TokenKind::RBrace); });
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
            // Statement evidence inside a "braced init" means it is really
            // a block — macro callbacks like LLVM_DEBUG({...}), capture-only
            // lambdas (clang-format calculateBraceTypes: semi/if/for/... in
            // an unknown brace forces BK_Block). Reclassify and continue as
            // statements; already-consumed tokens stay flat.
            if (k == TokenKind::Semicolon || k == TokenKind::ReturnKw ||
                k == TokenKind::IfKw || k == TokenKind::WhileKw || k == TokenKind::ForKw ||
                k == TokenKind::DoKw || k == TokenKind::SwitchKw) {
                nodes()[g].kind = SyntaxKind::CompoundStatement;
                nodes()[g].reclassified = true;
                run_container_items([&] { return at_eof() || at(TokenKind::RBrace); });
                expect_or_missing(TokenKind::RBrace);
                close(g);
                return;
            }
            // Preprocessor lines inside a braced list are neutral, exactly
            // as in calculateBraceTypes: consume the whole line and keep
            // classifying ("{a, \n#ifdef X\n b,\n#endif\n c}").
            if (k == TokenKind::PreprocessorHash) {
                while (pos_ + 1 < tokens().size() &&
                       has_flag(tokens()[pos_].flags, LexicalFlags::PreprocessorLine)) {
                    ++pos_;
                }
                continue;
            }
            if (is_sync_introducer(k)) {
                break; // structure that cannot be an initializer: bail
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
            case TokenKind::GreaterGreater:
                angle -= 2;
                if (angle <= 0) {
                    return i + 1;
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
        // True while the node consists of exactly one identifier; ':' then
        // makes it a goto label.
        bool label_candidate = false;
        // Linkage specification: `extern` then a string literal arms it; a
        // '{' then opens a namespace-like body (extern "C" { ... }).
        enum class Linkage : std::uint8_t { None, Extern, Armed };
        Linkage linkage = Linkage::None;

        while (!at_eof()) {
            const TokenKind k = kind();
            if (k == TokenKind::Semicolon) {
                advance();
                terminated = true;
                break;
            }
            // Enumerators are comma-separated siblings, not one declaration.
            if (k == TokenKind::Comma && enum_body_) {
                advance();
                terminated = true;
                break;
            }
            if (k == TokenKind::RBrace) {
                if (enum_body_) {
                    terminated = true; // the last enumerator ends at '}'
                } else {
                    mark_incomplete(n);
                }
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
                label_candidate = false;
                prev = TokenKind::RParen;
                header_pending = true;
                continue;
            }
            if (k == TokenKind::LBracket) {
                // clang-format tryToParseLambdaIntroducer: '[' is a lambda
                // when nothing identifier-like precedes it (start of the
                // statement, '=', ',', 'return') — then a '{' may follow
                // the capture list directly, with no parameter list.
                const bool lambda_introducer =
                    prev == TokenKind::EndOfFile || prev == TokenKind::Equals ||
                    prev == TokenKind::Comma || prev == TokenKind::ReturnKw;
                parse_bracket_group();
                macro_ident_only = false;
                label_candidate = false;
                prev = TokenKind::RBracket;
                if (lambda_introducer) {
                    header_pending = true;
                }
                continue;
            }
            if (k == TokenKind::LBrace) {
                if (linkage == Linkage::Armed) {
                    // extern "C" { ... }: namespace semantics — contents are
                    // top-level declarations, indent follows namespace style.
                    nodes()[n].kind = SyntaxKind::NamespaceDecl;
                    const SyntaxNodeId body = open(SyntaxKind::NamespaceBody);
                    advance();
                    run_container_items([&] { return at_eof() || at(TokenKind::RBrace); });
                    expect_or_missing(TokenKind::RBrace);
                    close(body);
                    terminated = true;
                    break;
                }
                if (header_pending) {
                    nodes()[n].kind = SyntaxKind::FunctionDefinition;
                    parse_compound_statement();
                    terminated = true;
                    break; // no ';' required after a body
                }
                parse_brace_group();
                label_candidate = false;
                prev = TokenKind::RBrace;
                continue;
            }
            // 'name:' with nothing else before it: a goto label, complete on
            // its own — the next statement is a sibling, not a continuation.
            if (k == TokenKind::Colon && label_candidate) {
                advance();
                terminated = true;
                break;
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
                    label_candidate = false;
                    prev = TokenKind::Greater;
                    continue;
                }
            }
            if (k == TokenKind::Question) {
                saw_question = true;
            }
            if (header_pending && k != TokenKind::Identifier && k != TokenKind::ColonColon &&
                k != TokenKind::Arrow && k != TokenKind::Amp && k != TokenKind::AmpAmp &&
                k != TokenKind::Star) {
                header_pending = false;
            }
            const bool first_identifier =
                prev == TokenKind::EndOfFile && k == TokenKind::Identifier;
            macro_ident_only = first_identifier && is_macro_name(token_text());
            label_candidate = first_identifier;
            if (first_identifier && token_text() == "extern") {
                linkage = Linkage::Extern;
            } else if (linkage == Linkage::Extern && k == TokenKind::StringLiteral) {
                linkage = Linkage::Armed;
            } else {
                linkage = Linkage::None;
            }
            advance();
            prev = k;
        }
        if (!terminated) {
            mark_incomplete(n); // no ';' (or body) yet — mid-typing state
        }
        close(n);
    }

    const Source& src_;
    const std::vector<Token>* tokens_view_ = nullptr; // sandbox mode
    std::string token_text_buffer_;
    std::uint32_t pos_ = 0;
    bool enum_body_ = false; // directly inside an enum's ClassBody
    SyntaxTree tree_;
    std::vector<SyntaxNodeId> stack_;

    // Preprocessor conditional reconciliation (design.md §276): each open
    // #if/#ifdef/#ifndef consumed at a container's item-loop level records the
    // owner loop's stack depth; a following #else/#elif restores to it so
    // alternative branches parse as siblings (one branch's net brace effect
    // counts, not the sum). See pp_open_item / run_container_items.
    struct PPFrame {
        std::uint32_t owner_depth;
    };
    std::vector<PPFrame> pp_frames_;
    static constexpr std::uint32_t kNoUnwind = 0xFFFFFFFFu;
    std::uint32_t unwind_target_ = kNoUnwind;
    bool unwinding() const { return unwind_target_ != kNoUnwind; }
    // Sandbox reparse only: set when an #else/#elif is reached whose #if opened
    // before the reparse span, so the block repair must escalate to a wider
    // container instead of guessing the conditional's brace balance.
    bool pp_escalate_ = false;
};

SyntaxTree parse(std::string_view text) {
    StringCharSource source{text};
    return Parser<StringCharSource>(source, lex(text)).run();
}

SyntaxTree parse(const Text& text) {
    TextCharSource source(text);
    return Parser<TextCharSource>(source, lex(text)).run();
}

SyntaxTree parse(const Text& text, LexOutput lexed) {
    TextCharSource source(text);
    return Parser<TextCharSource>(source, std::move(lexed)).run();
}

// ---- incremental reparse (design.md §17) -----------------------------------

namespace {

bool is_item_container(const SyntaxNode& n) {
    switch (n.kind) {
    case SyntaxKind::TranslationUnit:
    case SyntaxKind::NamespaceBody:
    case SyntaxKind::ClassBody:
    case SyntaxKind::CaseSection: return true;
    // A reclassified brace group consumed its early tokens with initializer
    // semantics; its item loop cannot be replayed uniformly.
    case SyntaxKind::CompoundStatement: return !n.reclassified;
    default: return false;
    }
}

struct RepairContext {
    SyntaxNodeId id;
    bool enum_body;
};

// Attempts the block repair inside container C. Returns the rebuilt container
// green node on success; null means escalate to the enclosing container. The
// old flat `nodes` (red cache) and `container_green` (C's green node) still hold
// OLD coordinates; the token vector is already the NEW stream. [guard_lo,
// tok_hi) is the old-coordinate damage window (guard included); old index
// j >= tok_hi lives at j + delta_tok in the new stream. Untouched child
// GreenRefs are reused by pointer — their relative encoding is splice-invariant.
GreenRef try_repair(const std::vector<SyntaxNode>& nodes, const std::vector<Token>& toks,
                    const RepairContext& ctx, const GreenRef& container_green,
                    std::uint32_t guard_lo, std::uint32_t tok_hi, std::int64_t delta_tok,
                    const Text& new_text) {
    const SyntaxNodeId c_id = ctx.id;
    const SyntaxKind c_kind = nodes[c_id].kind;
    const bool is_tu = c_kind == SyntaxKind::TranslationUnit;
    const auto new_count = static_cast<std::uint32_t>(toks.size());

    auto mapped = [&](std::uint32_t old_idx) {
        return static_cast<std::uint32_t>(old_idx + delta_tok);
    };

    // Items and the region they may occupy.
    std::vector<SyntaxNodeId> items;
    SyntaxNodeId label = kInvalidNode;    // CaseSection prologue
    SyntaxNodeId epilogue = kInvalidNode; // MissingToken for the closer
    for (SyntaxNodeId child : nodes[c_id].children) {
        if (nodes[child].kind == SyntaxKind::MissingToken) {
            epilogue = child;
        } else if (c_kind == SyntaxKind::CaseSection && child == nodes[c_id].children.front()) {
            label = child;
        } else {
            items.push_back(child);
        }
    }

    std::uint32_t lower = 0;
    if (c_kind == SyntaxKind::CaseSection) {
        if (label == kInvalidNode) {
            return nullptr;
        }
        lower = nodes[label].end_token;
    } else if (!is_tu) {
        lower = nodes[c_id].first_token + 1; // past the '{'
    }
    if (guard_lo < lower) {
        return nullptr;
    }

    // Post-loop boundary: where the container's item loop came to rest.
    std::uint32_t post_loop_old = 0;
    if (is_tu) {
        post_loop_old = 0; // unused; TU aligns on the new EOF directly
    } else if (epilogue != kInvalidNode) {
        post_loop_old = nodes[epilogue].first_token;
    } else if (c_kind == SyntaxKind::CaseSection) {
        // The stop token was not consumed; it is the first significant token
        // at or after the section's end — outside the window by containment.
        std::uint32_t j = nodes[c_id].end_token;
        if (j < tok_hi) {
            return nullptr;
        }
        while (is_trivia(toks[mapped(j)].kind)) {
            ++j;
        }
        post_loop_old = j;
    } else {
        post_loop_old = nodes[c_id].end_token - 1; // the consumed '}'
    }
    if (!is_tu && tok_hi > post_loop_old) {
        return nullptr; // the window reaches the container's own closer
    }

    // Item span [i, ...) to reparse and the alignment boundaries after it.
    // A zero-width item (e.g. an empty incomplete declaration) sitting exactly
    // at guard_lo occupies the damage-window boundary: its first_token == the
    // point the sandbox re-parses from, so keeping it as prefix would duplicate
    // it against the sandbox output. Require the item to start strictly before
    // guard_lo, which only excludes such degenerate boundary nodes (normal
    // items ending at guard_lo already start before it).
    std::size_t i = 0;
    while (i < items.size() && nodes[items[i]].end_token <= guard_lo &&
           nodes[items[i]].first_token < guard_lo) {
        ++i;
    }
    constexpr std::uint32_t kPostLoop = 0xFFFFFFFFu;
    std::vector<std::uint32_t> bounds;
    std::vector<std::uint32_t> bound_old; // parallel: old coords (kPostLoop = post-loop)
    for (std::size_t k = i; k < items.size(); ++k) {
        if (nodes[items[k]].first_token >= tok_hi) {
            bounds.push_back(mapped(nodes[items[k]].first_token));
            bound_old.push_back(nodes[items[k]].first_token);
        }
    }
    const std::uint32_t post_loop_new = is_tu ? new_count - 1 : mapped(post_loop_old);
    bounds.push_back(post_loop_new);
    bound_old.push_back(kPostLoop);

    // Sandbox start: the loop position right after the last untouched item.
    std::uint32_t start = i > 0 ? nodes[items[i - 1]].end_token : lower;
    while (is_trivia(toks[start].kind)) {
        ++start;
    }

    TextCharSource source(new_text);
    Parser<TextCharSource> sandbox(source, toks);
    auto result = sandbox.run_items(start, c_kind, ctx.enum_body, bounds);
    if (result.aligned_at == Parser<TextCharSource>::kNoBoundary) {
        return nullptr;
    }

    // Which old items the reparse replaced: [i, m).
    std::size_t m = items.size();
    for (std::size_t k = 0; k < bounds.size(); ++k) {
        if (bounds[k] == result.aligned_at && bound_old[k] != kPostLoop) {
            for (std::size_t it = i; it < items.size(); ++it) {
                if (nodes[items[it]].first_token == bound_old[k]) {
                    m = it;
                    break;
                }
            }
            break;
        }
    }

    // Green splice: rebuild C's child list, reusing the untouched prefix/suffix
    // child GreenRefs by pointer. C's green children line up 1:1 with its red
    // children (both DFS-preorder), partitioned [label?] + items + [epilogue?],
    // so the replaced item span [i, m) maps to green-child indices offset by the
    // label. Relative `leading`s are recomputed from each child's new-stream
    // first token; untouched children carry an unchanged gap and keep their
    // pointer.
    const std::size_t base = label != kInvalidNode ? 1 : 0;
    const std::size_t lo_ci = base + i;
    const std::size_t hi_ci = base + m;
    const std::vector<GreenChild>& old_children = container_green->children;
    const std::vector<SyntaxNodeId>& red_children = nodes[c_id].children;
    const std::uint32_t container_first = nodes[c_id].first_token; // unchanged by the edit

    std::vector<GreenChild> merged;
    merged.reserve(old_children.size() + result.nodes[0].children.size());
    std::uint32_t cursor = container_first; // running new-stream end position
    auto append = [&](const GreenRef& node, std::uint32_t new_first) {
        merged.push_back(GreenChild{new_first - cursor, node});
        cursor = new_first + node->width;
    };
    for (std::size_t k = 0; k < lo_ci; ++k) { // prefix: positions unchanged
        append(old_children[k].node, nodes[red_children[k]].first_token);
    }
    for (SyntaxNodeId sid : result.nodes[0].children) { // replacement: new stream
        append(green_from_flat_subtree(result.nodes, sid), result.nodes[sid].first_token);
    }
    for (std::size_t k = hi_ci; k < old_children.size(); ++k) { // suffix: shift by delta
        append(old_children[k].node,
               static_cast<std::uint32_t>(nodes[red_children[k]].first_token + delta_tok));
    }

    auto rebuilt = std::make_shared<GreenNode>();
    rebuilt->kind = container_green->kind;
    rebuilt->incomplete = container_green->incomplete;
    rebuilt->reclassified = container_green->reclassified;
    rebuilt->expected = container_green->expected;
    rebuilt->children = std::move(merged);
    // A CaseSection has no fixed closer — it ends at its last child; every other
    // container's closer just shifts by delta_tok, so its width does too.
    rebuilt->width = c_kind == SyntaxKind::CaseSection
                         ? cursor - container_first
                         : static_cast<std::uint32_t>(container_green->width + delta_tok);
    return rebuilt;
}

// True if the token range [lo, hi) contains a preprocessor conditional
// directive (#if/#ifdef/#ifndef/#else/#elif/#endif). `text` resolves the
// directive keyword spelling. Adding, deleting or retyping such a directive
// shifts the #if-frame nesting for every following sibling item, which the
// block repair reuses verbatim — so it forces a full reparse instead.
// The #if-frame model reshapes the tree only when an alternative branch forces
// an unwind, so it makes a block repair over the window at `guard_lo` unsafe in
// exactly two cases, both of which mean the reused prefix/suffix could carry a
// stale brace-frame context the local repair cannot rebuild:
//   * guard_lo sits inside an open conditional (an #if before it is not yet
//     closed) — an #else in that conditional, even before the window, unwinds
//     across it;
//   * an #else/#elif lies at or after guard_lo — its #if may be arbitrarily far
//     back and a brace edit shifts the depth it restores to.
// A file whose conditionals are all balanced-and-closed before guard_lo with no
// alternative in reach parses identically to the flat baseline, so this returns
// false and the fast incremental path runs. The caller full-reparses otherwise.
bool pp_repair_unsafe(const std::vector<Token>& toks, std::uint32_t guard_lo, const Text& text) {
    int depth = 0;
    for (std::uint32_t i = 0; i < toks.size(); ++i) {
        if (i == guard_lo && depth > 0) {
            return true; // window opens inside a conditional
        }
        if (toks[i].kind != TokenKind::PreprocessorHash) {
            continue;
        }
        std::uint32_t j = i + 1;
        while (j < toks.size() && is_trivia(toks[j].kind)) {
            ++j;
        }
        if (j >= toks.size()) {
            continue;
        }
        const TokenKind kw = toks[j].kind;
        const PPCat cat = kw == TokenKind::Identifier ? pp_classify(kw, text.substring(toks[j].range))
                                                      : pp_classify(kw, {});
        if (cat == PPCat::Alt && i >= guard_lo) {
            return true; // alternative branch within reach of the window
        }
        if (cat == PPCat::Open) {
            ++depth;
        } else if (cat == PPCat::Close) {
            depth = depth > 0 ? depth - 1 : 0;
        }
    }
    return false;
}

bool splice_touches_pp_conditional(const std::vector<Token>& toks, std::size_t lo, std::size_t hi,
                                   const Text& text) {
    for (std::size_t i = lo; i < hi && i < toks.size(); ++i) {
        if (toks[i].kind != TokenKind::PreprocessorHash) {
            continue;
        }
        std::size_t j = i + 1;
        while (j < toks.size() && is_trivia(toks[j].kind)) {
            ++j;
        }
        if (j >= toks.size()) {
            continue;
        }
        const TokenKind kw = toks[j].kind;
        const PPCat cat = kw == TokenKind::Identifier ? pp_classify(kw, text.substring(toks[j].range))
                                                      : pp_classify(kw, {});
        if (cat != PPCat::Other) {
            return true;
        }
    }
    return false;
}

} // namespace

void reparse(SyntaxTree& tree, std::vector<LexerState>& line_states, const Text& old_text,
             const Text& new_text, std::span<const TextEdit> edits) {
    if (edits.empty()) {
        return;
    }

    RelexSplice s = relex_scan(tree.tokens_, line_states, old_text, new_text, edits);

    // Token-identity fast path: the parser observes token kinds, flags and
    // identifier spellings — never byte positions (nodes hold token indices).
    // If the rescanned window matches the old one on those, the entire node
    // structure is reusable verbatim.
    const std::size_t old_span = s.stop_token - s.keep_tokens;
    bool identical = s.scanned.tokens.size() == old_span;
    for (std::size_t k = 0; identical && k < old_span; ++k) {
        const Token& a = tree.tokens_[s.keep_tokens + k];
        const Token& b = s.scanned.tokens[k];
        identical = a.kind == b.kind && a.flags == b.flags &&
                    (a.kind != TokenKind::Identifier ||
                     old_text.substring(a.range) == new_text.substring(b.range));
    }

    const std::uint32_t tok_lo = static_cast<std::uint32_t>(s.keep_tokens);
    const std::uint32_t tok_hi = static_cast<std::uint32_t>(s.stop_token);
    const std::int64_t delta_tok =
        static_cast<std::int64_t>(s.scanned.tokens.size()) - static_cast<std::int64_t>(old_span);

    // Read the old (about-to-be-replaced) and new token windows before the
    // splice is applied: a conditional directive on either side means the
    // #if-frame nesting may have shifted for following items.
    const bool pp_structure_changed =
        splice_touches_pp_conditional(tree.tokens_, tok_lo, tok_hi, old_text) ||
        splice_touches_pp_conditional(s.scanned.tokens, 0, s.scanned.tokens.size(), new_text);

    relex_apply(tree.tokens_, line_states, std::move(s));
    if (identical) {
        return;
    }
    if (pp_structure_changed) {
        LexOutput lexed;
        lexed.tokens = std::move(tree.tokens_);
        tree = parse(new_text, std::move(lexed));
        return;
    }

    // match_angles may look up to 200 significant tokens ahead, stopped only
    // unconditionally by ';', '{' or '}'. Extend the window start back to the
    // last such barrier so no reused item could have looked into the damage.
    std::uint32_t guard_lo = tok_lo;
    {
        int significant = 0;
        for (std::uint32_t j = tok_lo; j-- > 0;) {
            const TokenKind k = tree.tokens_[j].kind; // prefix: indices unchanged
            if (is_trivia(k)) {
                continue;
            }
            guard_lo = j;
            if (k == TokenKind::Semicolon || k == TokenKind::LBrace || k == TokenKind::RBrace ||
                ++significant >= 200) {
                break;
            }
        }
    }

    // A conditional whose #if-frame context the local repair cannot rebuild
    // (window inside an open conditional, or an #else/#elif in reach) forces a
    // full reparse. Files without a reachable alternative branch parse exactly
    // as the flat baseline, so this never fires for them.
    if (pp_repair_unsafe(tree.tokens_, guard_lo, new_text)) {
        LexOutput lexed;
        lexed.tokens = std::move(tree.tokens_);
        tree = parse(new_text, std::move(lexed));
        return;
    }

    // Descent from the root down to the deepest node holding the window,
    // tracking the parallel green spine (each node's GreenRef and the index it
    // occupies in its parent) so a successful repair can rebuild only that spine
    // and reuse every untouched subtree by pointer. Item-containers on the path
    // are the repair candidates, tried deepest-first.
    struct ChainEntry {
        RepairContext ctx;
        std::size_t depth; // index into path_green / path_index
    };
    std::vector<GreenRef> path_green{tree.green_root_};
    std::vector<std::uint32_t> path_index{0}; // path_green[k] sits at this index in path_green[k-1]
    std::vector<ChainEntry> chain;
    chain.push_back({{0, false}, 0});
    bool enum_ctx = false;
    SyntaxNodeId cur = 0;
    while (true) {
        SyntaxNodeId next = kInvalidNode;
        std::uint32_t next_index = 0;
        const std::vector<SyntaxNodeId>& kids = tree.nodes_[cur].children;
        for (std::uint32_t ci = 0; ci < kids.size(); ++ci) {
            const SyntaxNode& child = tree.nodes_[kids[ci]];
            if (child.kind != SyntaxKind::MissingToken && child.first_token <= guard_lo &&
                child.end_token >= tok_hi) {
                next = kids[ci];
                next_index = ci;
                break;
            }
        }
        if (next == kInvalidNode) {
            break;
        }
        if (tree.nodes_[next].kind == SyntaxKind::ClassBody) {
            // enum_body_ is scoped to the nearest ClassBody: derive it from
            // the class head's first significant token.
            const SyntaxNodeId decl = tree.nodes_[next].parent;
            std::uint32_t j = tree.nodes_[decl].first_token;
            while (is_trivia(tree.tokens_[j].kind)) {
                ++j;
            }
            enum_ctx = tree.tokens_[j].kind == TokenKind::EnumKw;
        }
        path_green.push_back(path_green.back()->children[next_index].node);
        path_index.push_back(next_index);
        if (is_item_container(tree.nodes_[next])) {
            chain.push_back({{next, enum_ctx}, path_green.size() - 1});
        }
        cur = next;
    }

    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        GreenRef spliced = try_repair(tree.nodes_, tree.tokens_, it->ctx, path_green[it->depth],
                                      guard_lo, tok_hi, delta_tok, new_text);
        if (!spliced) {
            continue;
        }
        // A CaseSection has no fixed closer, so its width can change by more or
        // less than delta_tok (the trailing-trivia boundary with its next
        // sibling moves). The direct parent must shift that next sibling's
        // leading to keep its absolute position at old + delta_tok; for
        // fixed-closer containers the adjustment is zero.
        const std::int64_t sibling_shift =
            delta_tok - (static_cast<std::int64_t>(spliced->width) -
                         static_cast<std::int64_t>(path_green[it->depth]->width));
        // Rebuild the spine above the repaired container, reusing sibling
        // GreenRefs by pointer; each ancestor grows by delta_tok.
        for (std::size_t k = it->depth; k-- > 0;) {
            auto parent = std::make_shared<GreenNode>(*path_green[k]);
            parent->width = static_cast<std::uint32_t>(parent->width + delta_tok);
            const std::uint32_t idx = path_index[k + 1];
            parent->children[idx].node = spliced;
            if (k == it->depth - 1 && idx + 1 < parent->children.size()) {
                GreenChild& next = parent->children[idx + 1];
                next.leading = static_cast<std::uint32_t>(next.leading + sibling_shift);
            }
            spliced = parent;
        }
        tree = flat_from_green(spliced, std::move(tree.tokens_));
        return;
    }

    // No bounded repair region: full reparse over the (already new) tokens.
    LexOutput lexed;
    lexed.tokens = std::move(tree.tokens_);
    SyntaxTree fresh = parse(new_text, std::move(lexed));
    tree = std::move(fresh);
}

} // namespace cind
