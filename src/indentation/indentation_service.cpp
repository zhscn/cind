#include "indentation/indentation_service.hpp"

#include "document/char_source.hpp"
#include "indentation/expression_continuation.hpp"
#include "syntax/pp_conditional.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <vector>

namespace cind {

std::string_view format_role_name(FormatRole role) {
    switch (role) {
    case FormatRole::File:
        return "File";
    case FormatRole::NamespaceBody:
        return "NamespaceBody";
    case FormatRole::TypeBody:
        return "TypeBody";
    case FormatRole::AccessSpecifierLabel:
        return "AccessSpecifierLabel";
    case FormatRole::FunctionBody:
        return "FunctionBody";
    case FormatRole::CompoundBody:
        return "CompoundBody";
    case FormatRole::LambdaBody:
        return "LambdaBody";
    case FormatRole::SingleStatementBody:
        return "SingleStatementBody";
    case FormatRole::ControlHeaderContinuation:
        return "ControlHeaderContinuation";
    case FormatRole::CaseLabel:
        return "CaseLabel";
    case FormatRole::CaseBody:
        return "CaseBody";
    case FormatRole::ConstructorInitializerIntro:
        return "ConstructorInitializerIntro";
    case FormatRole::ConstructorInitializerItem:
        return "ConstructorInitializerItem";
    case FormatRole::ParenContinuation:
        return "ParenContinuation";
    case FormatRole::BracketContinuation:
        return "BracketContinuation";
    case FormatRole::TemplateArgsContinuation:
        return "TemplateArgsContinuation";
    case FormatRole::BraceInit:
        return "BraceInit";
    case FormatRole::StatementContinuation:
        return "StatementContinuation";
    case FormatRole::PreprocessorDirective:
        return "PreprocessorDirective";
    case FormatRole::ClosingToken:
        return "ClosingToken";
    case FormatRole::PreservedRawString:
        return "PreservedRawString";
    case FormatRole::PreservedBlockComment:
        return "PreservedBlockComment";
    case FormatRole::Opaque:
        return "Opaque";
    }
    return "?";
}

namespace {

int display_width(std::string_view chars, int start_col, int tab_width) {
    int col = start_col;
    for (char c : chars) {
        col += c == '\t' ? tab_width - col % tab_width : 1;
    }
    return col;
}

std::string leading_whitespace(const TextCharSource& text, const Text& lines, std::uint32_t line) {
    TextRange content = lines.line_content_range(line);
    std::string out;
    for (std::uint32_t p = content.start.value; p < content.end.value; ++p) {
        const char c = text[p];
        if (c != ' ' && c != '\t') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

int line_indent_width(const TextCharSource& text, const Text& lines, std::uint32_t line,
                      int tab_width) {
    return display_width(leading_whitespace(text, lines, line), 0, tab_width);
}

int column_at(const TextCharSource& text, const Text& lines, TextOffset offset, int tab_width) {
    const std::uint32_t start = lines.line_start(lines.position(offset).line).value;
    int col = 0;
    for (std::uint32_t p = start; p < offset.value; ++p) {
        col += text[p] == '\t' ? tab_width - col % tab_width : 1;
    }
    return col;
}

// design.md §9.5: structural columns may become tabs; alignment beyond the
// structural part is always spaces.
std::string materialize(int structural_cols, int target_cols, const CppIndentStyle& style) {
    target_cols = std::max(target_cols, 0);
    if (!style.use_tabs) {
        return std::string(static_cast<std::size_t>(target_cols), ' ');
    }
    int tab_part = std::clamp(structural_cols, 0, target_cols);
    tab_part -= tab_part % style.tab_width;
    return std::string(static_cast<std::size_t>(tab_part / style.tab_width), '\t') +
           std::string(static_cast<std::size_t>(target_cols - tab_part), ' ');
}

// Deepest node whose range contains `offset`, treating ranges as closed so
// end-of-construct and end-of-file positions still resolve. The rightmost
// matching child wins at boundaries. Children are source-ordered, so each
// level is a binary search; the backward walk only skips zero-length
// MissingToken nodes sharing the boundary.
SyntaxNodeId query_node_at(const SyntaxTree& tree, TextOffset offset) {
    SyntaxNodeId current = tree.root();
    while (true) {
        const auto& children = tree.node(current).children;
        auto it = std::ranges::upper_bound(
            children, offset, {}, [&](SyntaxNodeId child) { return tree.node_range(child).start; });
        SyntaxNodeId next = kInvalidNode;
        while (it != children.begin()) {
            --it;
            if (tree.node(*it).kind == SyntaxKind::MissingToken) {
                continue;
            }
            if (tree.node_range(*it).end >= offset) {
                next = *it;
            }
            break;
        }
        if (next == kInvalidNode) {
            return current;
        }
        current = next;
    }
}

std::optional<SyntaxNodeId> find_ancestor(const SyntaxTree& tree, SyntaxNodeId from,
                                          SyntaxKind kind) {
    for (SyntaxNodeId id = from; id != kInvalidNode; id = tree.node(id).parent) {
        if (tree.node(id).kind == kind) {
            return id;
        }
    }
    return std::nullopt;
}

// First non-trivia token starting on the given line, if any.
std::optional<Token> first_significant_on_line(const SyntaxTree& tree, const Text& lines,
                                               std::uint32_t line) {
    TextRange range = lines.line_range(line);
    const auto& tokens = tree.tokens();
    auto it = std::ranges::lower_bound(tokens, range.start, {},
                                       [](const Token& t) { return t.range.start; });
    for (; it != tokens.end() && it->range.start < range.end; ++it) {
        if (!is_trivia(it->kind) && it->kind != TokenKind::EndOfFile) {
            return *it;
        }
    }
    return std::nullopt;
}

// Last non-trivia token ending at or before the offset.
std::optional<Token> last_significant_before(const SyntaxTree& tree, TextOffset offset) {
    const auto& tokens = tree.tokens();
    auto it =
        std::ranges::upper_bound(tokens, offset, {}, [](const Token& t) { return t.range.start; });
    while (it != tokens.begin()) {
        --it;
        if (!is_trivia(it->kind) && it->kind != TokenKind::EndOfFile && it->range.end <= offset) {
            return *it;
        }
    }
    return std::nullopt;
}

// Token whose range strictly contains the offset (for protection checks).
std::optional<Token> token_covering(const SyntaxTree& tree, TextOffset offset) {
    const auto& tokens = tree.tokens();
    auto it =
        std::ranges::upper_bound(tokens, offset, {}, [](const Token& t) { return t.range.start; });
    if (it == tokens.begin()) {
        return std::nullopt;
    }
    --it;
    if (it->range.start < offset && offset < it->range.end) {
        return *it;
    }
    return std::nullopt;
}

// clang-format IndentPPDirectives: BeforeHash. The leading column for the '#'
// directive that begins on `query_line`. Directives nest by preprocessor
// conditional depth (#if/#ifdef/#ifndef open a level, #endif closes it,
// #else/#elif render at the enclosing level); the outermost #ifndef/#define …
// trailing-#endif include guard is transparent, exactly as clang-format treats
// it. Returns 0 when the line is not a nested directive.
int pp_before_hash_column(const SyntaxTree& tree, const Text& text, std::uint32_t query_line,
                          int step) {
    const auto& tokens = tree.tokens();

    // The significant token right after a '#' — its directive keyword.
    auto keyword_index = [&](std::size_t hash) {
        std::size_t i = hash + 1;
        while (i < tokens.size() && is_trivia(tokens[i].kind)) {
            ++i;
        }
        return i;
    };
    auto category = [&](std::size_t hash) {
        const std::size_t i = keyword_index(hash);
        if (i >= tokens.size()) {
            return PPCat::Other;
        }
        const TokenKind k = tokens[i].kind;
        if (k != TokenKind::Identifier) {
            return pp_classify(k, {});
        }
        return pp_classify(k, text.substring(tokens[i].range));
    };

    // Directive starts in source order: a '#' that is the first significant
    // token on its physical line (this excludes the '#'/'##' stringize and
    // paste operators that appear inside a macro body).
    struct Dir {
        std::size_t hash;
        std::uint32_t line;
    };
    std::vector<Dir> dirs;
    bool line_start = true;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const TokenKind k = tokens[i].kind;
        if (k == TokenKind::Newline) {
            line_start = true;
            continue;
        }
        if (is_trivia(k)) {
            continue;
        }
        if (line_start && k == TokenKind::PreprocessorHash) {
            dirs.push_back({i, text.position(tokens[i].range.start).line});
        }
        line_start = false;
    }
    if (dirs.empty()) {
        return 0;
    }

    // Include-guard transparency: first directive #ifndef SYM, second #define
    // SYM (same symbol), the last directive an #endif with no code after it.
    bool guard = false;
    if (dirs.size() >= 3) {
        auto symbol_after = [&](std::size_t kw) -> std::string {
            std::size_t j = kw + 1;
            while (j < tokens.size() && is_trivia(tokens[j].kind)) {
                ++j;
            }
            if (j < tokens.size() && tokens[j].kind == TokenKind::Identifier) {
                return text.substring(tokens[j].range);
            }
            return {};
        };
        const std::size_t k0 = keyword_index(dirs.front().hash);
        const std::size_t k1 = keyword_index(dirs[1].hash);
        const std::size_t kl = keyword_index(dirs.back().hash);
        const bool shape = k0 < tokens.size() && tokens[k0].kind == TokenKind::Identifier &&
                           text.substring(tokens[k0].range) == "ifndef" && k1 < tokens.size() &&
                           tokens[k1].kind == TokenKind::Identifier &&
                           text.substring(tokens[k1].range) == "define" && kl < tokens.size() &&
                           tokens[kl].kind == TokenKind::Identifier &&
                           text.substring(tokens[kl].range) == "endif";
        if (shape) {
            const std::string s0 = symbol_after(k0);
            std::uint32_t last_sig_line = dirs.back().line;
            for (std::size_t i = tokens.size(); i-- > 0;) {
                const TokenKind k = tokens[i].kind;
                if (k == TokenKind::EndOfFile || is_trivia(k)) {
                    continue;
                }
                last_sig_line = text.position(tokens[i].range.start).line;
                break;
            }
            guard = !s0.empty() && s0 == symbol_after(k1) && last_sig_line <= dirs.back().line;
        }
    }

    int level = 0;
    for (const Dir& d : dirs) {
        int own = level;
        switch (category(d.hash)) {
        case PPCat::Open:
            own = level++;
            break;
        case PPCat::Alt:
            own = std::max(level - 1, 0);
            break;
        case PPCat::Close:
            own = (level = std::max(level - 1, 0));
            break;
        case PPCat::Other:
            own = level;
            break;
        }
        if (guard) {
            own = std::max(own - 1, 0);
        }
        if (d.line == query_line) {
            return own * step;
        }
    }
    return 0;
}

class IndentComputer {
public:
    IndentComputer(const DocumentSnapshot& snapshot, const SyntaxTree& tree, std::uint32_t line,
                   const CppIndentStyle& style)
        : snapshot_(snapshot), text_(snapshot.content()), lines_(snapshot.content()), tree_(tree),
          line_(line), style_(style) {}

    IndentDecision run() {
        if (check_protected()) {
            return std::move(decision_);
        }

        first_significant_ = first_significant_on_line(tree_, lines_, line_);
        if (!first_significant_ && comment_alignment()) {
            return std::move(decision_);
        }
        if (first_significant_) {
            trace("line starts with {}", token_kind_name(first_significant_->kind));
            switch (first_significant_->kind) {
            case TokenKind::RBrace:
            case TokenKind::RParen:
            case TokenKind::RBracket:
                if (closing_token()) {
                    return std::move(decision_);
                }
                break;
            case TokenKind::PreprocessorHash: {
                // BeforeHash indents the whole '#directive' by conditional
                // nesting depth; None and AfterHash keep '#' at column zero
                // (AfterHash's between-#-and-keyword spacing is intra-line, so
                // it does not change the leading column).
                int col = 0;
                if (style_.pp_directive_indent == CppIndentStyle::PPDirectiveIndent::BeforeHash) {
                    col = pp_before_hash_column(tree_, lines_, line_, style_.pp_step());
                }
                trace("preprocessor directive at conditional depth column {}", col);
                finish(FormatRole::PreprocessorDirective, col, col);
                return std::move(decision_);
            }
            case TokenKind::CaseKw:
            case TokenKind::DefaultKw:
                if (case_label()) {
                    return std::move(decision_);
                }
                break;
            case TokenKind::PublicKw:
            case TokenKind::ProtectedKw:
            case TokenKind::PrivateKw:
                if (access_label()) {
                    return std::move(decision_);
                }
                break;
            case TokenKind::Colon:
                if (ctor_intro()) {
                    return std::move(decision_);
                }
                break;
            default:
                break;
            }
        }

        generic();
        return std::move(decision_);
    }

private:
    template <typename... Args> void trace(std::format_string<Args...> fmt, Args&&... args) {
        decision_.trace.push_back(std::format(fmt, std::forward<Args>(args)...));
    }

    std::uint32_t line_of(TextOffset offset) const { return lines_.position(offset).line; }

    int indent_of_line(std::uint32_t line) const {
        return line_indent_width(text_, lines_, line, style_.tab_width);
    }

    void finish(FormatRole role, int structural_cols, int target_cols) {
        // A line that opens re-opened #else scopes (PPReopenedScope) computes
        // its indent at the scope's parent and adds one level per phantom: the
        // content continues the body the previous branch closed.
        structural_cols += reopened_extra_;
        target_cols += reopened_extra_;
        decision_.role = role;
        decision_.target_column = std::max(target_cols, 0);
        decision_.indentation_text = materialize(structural_cols, target_cols, style_);
        trace("role {} -> column {}", format_role_name(role), decision_.target_column);
    }

    // A braced body is measured from the line where its owning construct
    // starts, not where '{' landed: a wrapped signature or condition puts
    // '{' on a continuation line, and the body must not shift with it.
    TextOffset anchor_origin(SyntaxNodeId node) const {
        switch (tree_.node(node).kind) {
        case SyntaxKind::CompoundStatement:
        case SyntaxKind::ClassBody:
        case SyntaxKind::NamespaceBody:
        case SyntaxKind::BraceGroup:
            break;
        default:
            return tree_.node_range(node).start;
        }
        // One step only: the enclosing construct whose header owns this
        // brace. Statements further out contain this one as a body and must
        // not pull the anchor to their own line.
        const SyntaxNodeId parent = tree_.node(node).parent;
        if (parent != kInvalidNode) {
            switch (tree_.node(parent).kind) {
            case SyntaxKind::FunctionDefinition:
            case SyntaxKind::OpaqueDeclaration:
            case SyntaxKind::ClassDecl:
            case SyntaxKind::NamespaceDecl:
            case SyntaxKind::IfStatement:
            case SyntaxKind::ElseClause:
            case SyntaxKind::ForStatement:
            case SyntaxKind::WhileStatement:
            case SyntaxKind::DoStatement:
            case SyntaxKind::SwitchStatement:
                return tree_.node_range(parent).start;
            default:
                break;
            }
        }
        return tree_.node_range(node).start;
    }

    // clang-format TT_FunctionDeclarationName approximation: this line
    // begins the declarator name of a declaration-scope function whose
    // parameter list opens on the same line, and everything from the
    // declaration start up to that '(' is name/type material — no '=',
    // ',' or expression operators.
    bool wrapped_declarator_name_line(SyntaxNodeId node) const {
        const SyntaxNode& n = tree_.node(node);
        if (n.kind != SyntaxKind::FunctionDefinition && n.kind != SyntaxKind::OpaqueDeclaration) {
            return false;
        }
        if (n.parent == kInvalidNode) {
            return false;
        }
        switch (tree_.node(n.parent).kind) {
        case SyntaxKind::TranslationUnit:
        case SyntaxKind::NamespaceBody:
        case SyntaxKind::ClassBody:
            break;
        default:
            return false;
        }
        SyntaxNodeId params = kInvalidNode;
        for (SyntaxNodeId child : n.children) {
            if (tree_.node(child).kind == SyntaxKind::ParenGroup) {
                params = child;
                break;
            }
        }
        if (params == kInvalidNode) {
            return false;
        }
        const auto& tokens = tree_.tokens();
        const std::uint32_t open = tree_.node(params).first_token;
        if (lines_.position(tokens[open].range.start).line != line_) {
            return false;
        }
        TokenKind prev = TokenKind::EndOfFile;
        for (std::uint32_t i = n.first_token; i < open; ++i) {
            // Template arguments (and attribute brackets) are opaque
            // children of the declaration; skip their whole span.
            bool in_child = false;
            for (SyntaxNodeId child : n.children) {
                const SyntaxNode& c = tree_.node(child);
                if (c.first_token <= i && i < c.end_token) {
                    i = c.end_token - 1;
                    in_child = c.kind == SyntaxKind::TemplateArgumentList ||
                               c.kind == SyntaxKind::BracketGroup;
                    break;
                }
            }
            if (in_child) {
                prev = TokenKind::Greater;
                continue;
            }
            const Token& t = tokens[i];
            if (is_trivia(t.kind)) {
                continue;
            }
            switch (t.kind) {
            case TokenKind::Identifier:
            case TokenKind::ColonColon:
            case TokenKind::Arrow:
            case TokenKind::OperatorKw:
                break;
            // Return-type decoration; also legal as an operator name.
            case TokenKind::Star:
            case TokenKind::Amp:
            case TokenKind::AmpAmp:
            case TokenKind::Tilde:
                break;
            case TokenKind::Equals:
            case TokenKind::Less:
            case TokenKind::Greater:
                if (prev != TokenKind::OperatorKw) {
                    return false;
                }
                break;
            default:
                // Any other operator spelling only as `operator@`'s name.
                if (prev != TokenKind::OperatorKw ||
                    (!is_operator_spelling(t.kind) && t.kind != TokenKind::Punctuator)) {
                    return false;
                }
                break;
            }
            prev = t.kind;
        }
        return true;
    }

    // T2.5 alignment: content following the open bracket on its own line
    // makes wrapped lines align with that first piece of content.
    std::optional<int> aligned_continuation(SyntaxNodeId group) const {
        const SyntaxNode& g = tree_.node(group);
        const auto& tokens = tree_.tokens();
        const std::uint32_t open_line = lines_.position(tokens[g.first_token].range.start).line;
        for (std::uint32_t i = g.first_token + 1; i < g.end_token; ++i) {
            if (is_trivia(tokens[i].kind)) {
                continue;
            }
            if (lines_.position(tokens[i].range.start).line != open_line) {
                return std::nullopt;
            }
            return column_at(text_, lines_, tokens[i].range.start, style_.tab_width);
        }
        return std::nullopt;
    }

    // A comment-only line belongs to the code it annotates only when it is
    // already column-aligned with it (clang-format's original-column rule
    // in distributeComments): then it adopts that line's decision. An
    // unaligned comment keeps the enclosing structure's indent via the
    // generic path.
    bool comment_alignment() {
        TextRange content = lines_.line_content_range(line_);
        const auto& tokens = tree_.tokens();
        auto it = std::ranges::lower_bound(tokens, content.start, {},
                                           [](const Token& t) { return t.range.start; });
        while (it != tokens.end() && it->range.start < content.end &&
               it->kind == TokenKind::Whitespace) {
            ++it;
        }
        if (it == tokens.end() || it->range.start >= content.end ||
            (it->kind != TokenKind::LineComment && it->kind != TokenKind::BlockComment)) {
            return false;
        }
        for (std::uint32_t next = line_ + 1; next < lines_.line_count(); ++next) {
            auto tok = first_significant_on_line(tree_, lines_, next);
            if (!tok) {
                continue; // blank or another comment line
            }
            IndentDecision adopted = IndentComputer(snapshot_, tree_, next, style_).run();
            if (adopted.preserve || adopted.target_column != indent_of_line(line_)) {
                return false; // not aligned with what follows: keep structure
            }
            adopted.trace.insert(adopted.trace.begin(),
                                 std::format("comment line aligned with line {}", next + 1));
            decision_ = std::move(adopted);
            return true;
        }
        return false;
    }

    bool check_protected() {
        TextOffset line_start = lines_.line_start(line_);
        auto covering = token_covering(tree_, line_start);
        if (!covering) {
            return false;
        }
        if (covering->kind == TokenKind::RawStringLiteral) {
            trace("line starts inside a raw string literal; indentation is content");
            decision_.preserve = true;
            decision_.role = FormatRole::PreservedRawString;
            return true;
        }
        if (covering->kind == TokenKind::BlockComment) {
            trace("line starts inside a block comment; preserved");
            decision_.preserve = true;
            decision_.role = FormatRole::PreservedBlockComment;
            return true;
        }
        return false;
    }

    // The closing token of a group aligns with the line that opened it.
    bool closing_token() {
        SyntaxNodeId owner = query_node_at(tree_, first_significant_->range.start);
        switch (tree_.node(owner).kind) {
        case SyntaxKind::CompoundStatement:
        case SyntaxKind::BraceGroup:
        case SyntaxKind::NamespaceBody:
        case SyntaxKind::ClassBody:
        case SyntaxKind::ParenGroup:
        case SyntaxKind::BracketGroup:
            break;
        default:
            return false;
        }
        TextOffset opening = anchor_origin(owner);
        int target = indent_of_line(line_of(opening));
        decision_.anchor = opening;
        trace("closing token of {} anchored at line {}", syntax_kind_name(tree_.node(owner).kind),
              line_of(opening) + 1);
        finish(FormatRole::ClosingToken, target, target);
        return true;
    }

    bool case_label() {
        SyntaxNodeId node = query_node_at(tree_, first_significant_->range.start);
        auto switch_node = find_ancestor(tree_, node, SyntaxKind::SwitchStatement);
        if (!switch_node) {
            return false;
        }
        TextOffset anchor = tree_.node_range(*switch_node).start;
        int base = indent_of_line(line_of(anchor));
        int target = base + (style_.indent_case_label ? style_.indent_width : 0);
        decision_.anchor = anchor;
        trace("case label in switch at line {}; style.indent_case_label = {}", line_of(anchor) + 1,
              style_.indent_case_label);
        finish(FormatRole::CaseLabel, target, target);
        return true;
    }

    bool access_label() {
        // `public Base` in a base clause is not a label: the keyword must be
        // followed by (identifiers and) a single ':' (Qt's "public slots:").
        std::size_t p = first_significant_->range.end.value;
        while (p < text_.size() && (text_[p] == ' ' || text_[p] == '\t' || text_[p] == '_' ||
                                    (std::isalnum(static_cast<unsigned char>(text_[p])) != 0))) {
            ++p;
        }
        if (p >= text_.size() || text_[p] != ':' || (p + 1 < text_.size() && text_[p + 1] == ':')) {
            return false;
        }
        SyntaxNodeId node = query_node_at(tree_, first_significant_->range.start);
        auto class_node = find_ancestor(tree_, node, SyntaxKind::ClassDecl);
        if (!class_node) {
            return false;
        }
        TextOffset anchor = tree_.node_range(*class_node).start;
        int target = indent_of_line(line_of(anchor)) + style_.access_specifier_offset;
        decision_.anchor = anchor;
        trace("access specifier in type declared at line {}; offset = {}", line_of(anchor) + 1,
              style_.access_specifier_offset);
        finish(FormatRole::AccessSpecifierLabel, target, target);
        return true;
    }

    bool ctor_intro() {
        SyntaxNodeId node = query_node_at(tree_, first_significant_->range.start);
        if (tree_.node(node).kind != SyntaxKind::CtorInitializerList) {
            return false;
        }
        auto fn = find_ancestor(tree_, node, SyntaxKind::FunctionDefinition);
        int base = fn ? indent_of_line(line_of(tree_.node_range(*fn).start)) : 0;
        if (fn) {
            decision_.anchor = tree_.node_range(*fn).start;
        }
        trace("constructor initializer intro; continuation from the declarator line");
        finish(FormatRole::ConstructorInitializerIntro, base, base + style_.continuation_indent);
        return true;
    }

    void ctor_items(SyntaxNodeId list) {
        auto fn = find_ancestor(tree_, list, SyntaxKind::FunctionDefinition);
        int fn_indent = fn ? indent_of_line(line_of(tree_.node_range(*fn).start)) : 0;
        using Style = CppIndentStyle::ConstructorInitializerStyle;
        switch (style_.constructor_initializers) {
        case Style::AlignFirstInitializer: {
            for (SyntaxNodeId child : tree_.node(list).children) {
                if (tree_.node(child).kind == SyntaxKind::CtorInitializer) {
                    TextOffset anchor = tree_.node_range(child).start;
                    decision_.anchor = anchor;
                    int col = column_at(text_, lines_, anchor, style_.tab_width);
                    trace("style AlignFirstInitializer: aligned to first initializer at "
                          "line {} column {}",
                          line_of(anchor) + 1, col);
                    finish(FormatRole::ConstructorInitializerItem, fn_indent, col);
                    return;
                }
            }
            trace("style AlignFirstInitializer but no initializer yet; continuation");
            finish(FormatRole::ConstructorInitializerItem, fn_indent,
                   fn_indent + style_.continuation_indent);
            return;
        }
        case Style::AlignAfterColon: {
            TextOffset colon = tree_.node_range(list).start;
            decision_.anchor = colon;
            int col = column_at(text_, lines_, colon, style_.tab_width) + 2;
            trace("style AlignAfterColon");
            finish(FormatRole::ConstructorInitializerItem, fn_indent, col);
            return;
        }
        case Style::AlignWithColon: {
            // Comma-prepended items (BreakConstructorInitializers:
            // BeforeComma): the ',' sits exactly under the ':'.
            TextOffset colon = tree_.node_range(list).start;
            decision_.anchor = colon;
            int col = column_at(text_, lines_, colon, style_.tab_width);
            trace("style AlignWithColon (comma-prepended items)");
            finish(FormatRole::ConstructorInitializerItem, fn_indent, col);
            return;
        }
        case Style::NormalIndent:
            trace("style NormalIndent relative to the declarator line");
            finish(FormatRole::ConstructorInitializerItem, fn_indent + style_.indent_width,
                   fn_indent + style_.indent_width);
            return;
        case Style::ContinuationIndent:
            trace("style ContinuationIndent relative to the declarator line");
            finish(FormatRole::ConstructorInitializerItem, fn_indent + style_.continuation_indent,
                   fn_indent + style_.continuation_indent);
            return;
        }
    }

    void generic() {
        TextOffset query =
            first_significant_ ? first_significant_->range.start : lines_.line_start(line_);
        SyntaxNodeId node;
        if (first_significant_) {
            node = query_node_at(tree_, query);
        } else {
            // design.md §10.2 steps 6-9: anchor on the last non-trivia block
            // before the caret; blocks that are complete and already ended
            // defer to their parent.
            auto prev = last_significant_before(tree_, query);
            node = prev ? query_node_at(tree_, prev->range.start) : query_node_at(tree_, query);
            while (node != tree_.root() && !tree_.node(node).incomplete &&
                   tree_.node_range(node).end <= query) {
                trace("{} before caret is complete; deferring to its parent",
                      syntax_kind_name(tree_.node(node).kind));
                node = tree_.node(node).parent;
            }
            if (tree_.node(node).incomplete) {
                trace("{} before caret is incomplete; it controls the new line",
                      syntax_kind_name(tree_.node(node).kind));
            }
        }

        // Deepest ancestor that starts on an earlier line: its opening line
        // is the base everything else is relative to. Crossing a re-opened
        // #else scope that starts on this line means this line is the first
        // content of that phantom scope: one extra level per phantom.
        SyntaxNodeId relevant = node;
        while (relevant != kInvalidNode && line_of(tree_.node_range(relevant).start) >= line_) {
            if (tree_.node(relevant).kind == SyntaxKind::PPReopenedScope) {
                reopened_extra_ += style_.indent_width;
                trace("first line of a re-opened #else scope; one level deeper");
            }
            relevant = tree_.node(relevant).parent;
        }
        if (relevant == kInvalidNode) {
            trace("no enclosing structure; column zero");
            finish(FormatRole::File, 0, 0);
            return;
        }

        const SyntaxNode& a = tree_.node(relevant);
        const TextOffset origin = anchor_origin(relevant);
        const std::uint32_t opening_line = line_of(origin);
        const int base = indent_of_line(opening_line);
        const int w = style_.indent_width;
        const int cont = style_.continuation_indent;
        decision_.anchor = origin;
        trace("controlling block: {} anchored at line {} (indent {})", syntax_kind_name(a.kind),
              opening_line + 1, base);

        switch (a.kind) {
        case SyntaxKind::TranslationUnit:
            finish(FormatRole::File, 0, 0);
            return;
        case SyntaxKind::NamespaceBody: {
            using NI = CppIndentStyle::NamespaceIndentation;
            bool nested = false;
            for (SyntaxNodeId up = a.parent; up != kInvalidNode; up = tree_.node(up).parent) {
                if (tree_.node(up).kind == SyntaxKind::NamespaceBody) {
                    nested = true;
                    break;
                }
            }
            const bool indents = style_.namespace_indentation == NI::All ||
                                 (style_.namespace_indentation == NI::Inner && nested);
            trace("style.namespace_indentation = {}, nested = {}",
                  style_.namespace_indentation == NI::None    ? "None"
                  : style_.namespace_indentation == NI::Inner ? "Inner"
                                                              : "All",
                  nested);
            finish(FormatRole::NamespaceBody, base, base + (indents ? w : 0));
            return;
        }
        case SyntaxKind::ClassBody:
            trace("style.indent_type_body = {}", style_.indent_type_body);
            finish(FormatRole::TypeBody, base + (style_.indent_type_body ? w : 0),
                   base + (style_.indent_type_body ? w : 0));
            return;
        case SyntaxKind::CompoundStatement: {
            const SyntaxNode& compound = a;
            // Between case sections of a switch body, indent as the previous
            // section's body.
            if (compound.parent != kInvalidNode &&
                tree_.node(compound.parent).kind == SyntaxKind::SwitchStatement) {
                SyntaxNodeId prev_section = kInvalidNode;
                for (SyntaxNodeId child : compound.children) {
                    if (tree_.node(child).kind == SyntaxKind::CaseSection &&
                        tree_.node_range(child).end <= query) {
                        prev_section = child;
                    }
                }
                if (prev_section != kInvalidNode) {
                    int label_indent =
                        indent_of_line(line_of(tree_.node_range(prev_section).start));
                    trace("after a case section; style.indent_case_body = {}",
                          style_.indent_case_body);
                    finish(FormatRole::CaseBody, label_indent + (style_.indent_case_body ? w : 0),
                           label_indent + (style_.indent_case_body ? w : 0));
                    return;
                }
            }
            FormatRole role = FormatRole::CompoundBody;
            if (compound.parent != kInvalidNode) {
                SyntaxKind parent_kind = tree_.node(compound.parent).kind;
                if (parent_kind == SyntaxKind::FunctionDefinition) {
                    role = FormatRole::FunctionBody;
                } else if (parent_kind == SyntaxKind::ParenGroup ||
                           parent_kind == SyntaxKind::BracketGroup ||
                           parent_kind == SyntaxKind::BraceGroup ||
                           parent_kind == SyntaxKind::OpaqueDeclaration) {
                    role = FormatRole::LambdaBody;
                }
            }
            finish(role, base + w, base + w);
            return;
        }
        case SyntaxKind::BraceGroup: {
            if (expression_engine(relevant, FormatRole::BraceInit)) {
                return;
            }
            if (style_.align_open_bracket) {
                if (auto col = aligned_continuation(relevant)) {
                    trace("open brace has trailing content; aligning with it");
                    finish(FormatRole::BraceInit, base, *col);
                    return;
                }
            }
            const int step = style_.brace_init_continuation ? cont : w;
            finish(FormatRole::BraceInit, base + step, base + step);
            return;
        }
        case SyntaxKind::CaseSection:
            trace("statement in a case section; style.indent_case_body = {}",
                  style_.indent_case_body);
            finish(FormatRole::CaseBody, base + (style_.indent_case_body ? w : 0),
                   base + (style_.indent_case_body ? w : 0));
            return;
        case SyntaxKind::ParenGroup:
        case SyntaxKind::BracketGroup:
        case SyntaxKind::TemplateArgumentList: {
            const FormatRole role = a.kind == SyntaxKind::ParenGroup ? FormatRole::ParenContinuation
                                    : a.kind == SyntaxKind::BracketGroup
                                        ? FormatRole::BracketContinuation
                                        : FormatRole::TemplateArgsContinuation;
            if (expression_engine(relevant, role)) {
                return;
            }
            if (style_.align_open_bracket) {
                if (auto col = aligned_continuation(relevant)) {
                    trace("open bracket has trailing content; aligning with it");
                    finish(role, base, *col);
                    return;
                }
            }
            finish(role, base + cont, base + cont);
            return;
        }
        case SyntaxKind::CtorInitializerList:
            ctor_items(relevant);
            return;
        case SyntaxKind::FunctionDefinition: {
            // A pending initializer list with no body yet: new lines continue
            // the initializer items even though the list node ended earlier.
            SyntaxNodeId list = kInvalidNode;
            bool body_before_query = false;
            for (SyntaxNodeId child : a.children) {
                SyntaxKind k = tree_.node(child).kind;
                if (k == SyntaxKind::CtorInitializerList && tree_.node_range(child).end <= query) {
                    list = child;
                }
                if (k == SyntaxKind::CompoundStatement && tree_.node_range(child).start < query) {
                    body_before_query = true;
                }
            }
            if (list != kInvalidNode && !body_before_query) {
                trace("continuing a pending constructor initializer list");
                ctor_items(list);
                return;
            }
            if (!style_.indent_wrapped_function_names && wrapped_declarator_name_line(relevant)) {
                trace("wrapped declarator name; kept at the declaration indent");
                finish(FormatRole::StatementContinuation, base, base);
                return;
            }
            if (expression_engine(relevant, FormatRole::StatementContinuation)) {
                return;
            }
            finish(FormatRole::StatementContinuation, base + cont, base + cont);
            return;
        }
        case SyntaxKind::IfStatement:
            if (first_significant_ && first_significant_->kind == TokenKind::ElseKw) {
                trace("else aligns with its if");
                finish(FormatRole::Opaque, base, base);
                return;
            }
            finish(FormatRole::SingleStatementBody, base + w, base + w);
            return;
        case SyntaxKind::ElseClause:
        case SyntaxKind::ForStatement:
        case SyntaxKind::WhileStatement:
            finish(FormatRole::SingleStatementBody, base + w, base + w);
            return;
        case SyntaxKind::DoStatement:
            if (first_significant_ && first_significant_->kind == TokenKind::WhileKw) {
                trace("while of do-while aligns with its do");
                finish(FormatRole::Opaque, base, base);
                return;
            }
            finish(FormatRole::SingleStatementBody, base + w, base + w);
            return;
        case SyntaxKind::SwitchStatement:
            finish(FormatRole::Opaque, base + w, base + w);
            return;
        case SyntaxKind::PreprocessorDirective:
            // A '\'-continued macro body indents like a block, not like an
            // expression continuation (clang-format formats it this way).
            finish(FormatRole::PreprocessorDirective, base + w, base + w);
            return;
        case SyntaxKind::PPReopenedScope:
            // Interior of a phantom scope re-opened at #else/#elif: content
            // continues the body the previous branch closed. Its first line
            // carries the body indent (via the reopened-extra bump above), so
            // later lines align flat with it and the re-closing '}' drops
            // back one level.
            if (first_significant_ && first_significant_->kind == TokenKind::RBrace &&
                tree_.node_range(relevant).end == first_significant_->range.end) {
                trace("closer of a re-opened #else scope; one level back");
                finish(FormatRole::ClosingToken, base - w, base - w);
                return;
            }
            trace("inside a re-opened #else scope; aligning with its first line");
            finish(FormatRole::Opaque, base, base);
            return;
        case SyntaxKind::Error: {
            trace("inside an error node; preserving the previous line's indent");
            int prev = line_ > 0 ? indent_of_line(line_ - 1) : 0;
            finish(FormatRole::Opaque, prev, prev);
            return;
        }
        default: // opaque declarations, labels, class heads: continuation
            if (!style_.indent_wrapped_function_names && wrapped_declarator_name_line(relevant)) {
                trace("wrapped declarator name; kept at the declaration indent");
                finish(FormatRole::StatementContinuation, base, base);
                return;
            }
            if (expression_engine(relevant, FormatRole::StatementContinuation)) {
                return;
            }
            finish(FormatRole::StatementContinuation, base + cont, base + cont);
            return;
        }
    }

    // T3: replay clang-format's continuation column rules over the actual
    // statement layout (design.md §10.2). False = fall back to the T2 table.
    bool expression_engine(SyntaxNodeId controlling, FormatRole role) {
        auto column = expression_continuation_column(snapshot_, tree_, line_, controlling, style_,
                                                     decision_.trace);
        if (!column) {
            return false;
        }
        finish(role, *column, *column);
        return true;
    }

    const DocumentSnapshot& snapshot_;
    TextCharSource text_;
    const Text& lines_;
    const SyntaxTree& tree_;
    std::uint32_t line_;
    CppIndentStyle style_;
    std::optional<Token> first_significant_;
    IndentDecision decision_;
    // Levels added for re-opened #else scopes that start on this line;
    // applied by finish(). Only generic()'s relevant-walk sets it.
    int reopened_extra_ = 0;
};

} // namespace

IndentDecision compute_line_indent(const DocumentSnapshot& snapshot, const SyntaxTree& tree,
                                   std::uint32_t line, const CppIndentStyle& style) {
    return IndentComputer(snapshot, tree, line, style).run();
}

} // namespace cind
