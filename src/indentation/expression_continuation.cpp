#include "indentation/expression_continuation.hpp"

#include "document/char_source.hpp"

#include <algorithm>
#include <format>

// Port of the C++ subset of clang-format's expression machinery:
//   - TokenAnnotator.cpp ExpressionParser (fake parens by operator precedence)
//   - ContinuationIndenter.cpp addTokenOnCurrentLine / addTokenOnNewLine /
//     moveStateToNextToken / moveStatePastFakeLParens / moveStatePastScope* /
//     getNewLineColumn
// Deliberate simplifications: comments are skipped (comment lines are decided
// by the comment-adoption rule before this engine runs), block bodies inside
// the region are opaque (clang-format's child lines), and only the styles the
// kernel models participate. Already-laid-out tokens use their actual file
// columns, so no penalties or break decisions are needed.

namespace cind {

namespace {

// clang/Basic/OperatorPrecedence.h numbering.
enum Precedence : int {
    kPrecUnknown = 0,
    kPrecComma = 1,
    kPrecAssignment = 2,
    kPrecConditional = 3,
    kPrecLogicalOr = 4,
    kPrecPointerToMember = 15,
    kPrecUnary = 16,
    kPrecArrowAndPeriod = 17,
};

struct SimToken {
    TokenKind kind = TokenKind::Invalid;
    int length = 0; // display width == byte length for the tokens that matter
    int column = 0;
    bool newline_before = false;

    enum class Scope : std::uint8_t { None, Open, Close };
    Scope scope = Scope::None;
    int close_count = 1;       // '>>' closes two template groups at once
    bool block = false;        // scope token of a skipped code block
    bool braced_init = false;  // scope token of a braced initializer list
    bool multi_param = false;  // opener with more than one comma at depth 1

    bool binary = false; // acts as a binary operator
    bool unary = false;
    int precedence = -1;
    int operator_index = 0;    // index among same-level operators
    bool next_operator = false;
    bool starts_binary_expression = false;
    bool ternary_question = false;
    bool ternary_colon = false;
    bool ctor_colon = false;
    bool member_access = false;
    bool lessless = false;

    std::vector<int> fake_lparens; // applied back-to-front
    int fake_rparens = 0;
};

bool is_string(TokenKind kind) {
    return kind == TokenKind::StringLiteral || kind == TokenKind::RawStringLiteral;
}

bool is_operand_end(const SimToken& t) {
    return t.scope == SimToken::Scope::Close || t.kind == TokenKind::Identifier ||
           t.kind == TokenKind::Number || is_string(t.kind) ||
           t.kind == TokenKind::CharacterLiteral;
}

int binary_precedence(const SimToken& t) {
    switch (t.kind) {
    case TokenKind::Comma: return kPrecComma;
    case TokenKind::Equals:
    case TokenKind::PlusEqual:
    case TokenKind::MinusEqual:
    case TokenKind::StarEqual:
    case TokenKind::SlashEqual:
    case TokenKind::PercentEqual:
    case TokenKind::AmpEqual:
    case TokenKind::PipeEqual:
    case TokenKind::CaretEqual:
    case TokenKind::LessLessEqual:
    case TokenKind::GreaterGreaterEqual: return kPrecAssignment;
    case TokenKind::PipePipe: return kPrecLogicalOr;
    case TokenKind::AmpAmp: return 5;
    case TokenKind::Pipe: return 6;
    case TokenKind::Caret: return 7;
    case TokenKind::Amp: return 8;
    case TokenKind::EqualEqual:
    case TokenKind::ExclaimEqual: return 9;
    case TokenKind::Less:
    case TokenKind::Greater:
    case TokenKind::LessEqual:
    case TokenKind::GreaterEqual: return 10; // Relational
    case TokenKind::Spaceship: return 11;
    case TokenKind::LessLess:
    case TokenKind::GreaterGreater: return 12;
    case TokenKind::Plus:
    case TokenKind::Minus: return 13;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return 14;
    case TokenKind::PeriodStar:
    case TokenKind::ArrowStar: return kPrecPointerToMember;
    case TokenKind::Period:
    case TokenKind::Arrow: return kPrecArrowAndPeriod;
    default: return -1;
    }
}

bool is_unary_spelling(const SimToken& t) {
    switch (t.kind) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Amp:
    case TokenKind::Exclaim:
    case TokenKind::Tilde:
    case TokenKind::PlusPlus:
    case TokenKind::MinusMinus: return true;
    default: return false;
    }
}

// ---- fake paren assignment (TokenAnnotator::ExpressionParser port) ---------

class FakeParenAssigner {
public:
    explicit FakeParenAssigner(std::vector<SimToken>& tokens) : tokens_(tokens) {}

    void run() { parse(0); }

private:
    std::vector<SimToken>& tokens_;
    std::size_t pos_ = 0;

    SimToken* cur() { return pos_ < tokens_.size() ? &tokens_[pos_] : nullptr; }
    void next() { ++pos_; }

    int current_precedence() {
        SimToken* t = cur();
        if (!t) {
            return -1;
        }
        if (t->ternary_question || t->ternary_colon) {
            return kPrecConditional;
        }
        if (t->kind == TokenKind::Semicolon) {
            return 0;
        }
        if (t->member_access) {
            return kPrecArrowAndPeriod;
        }
        if (t->binary || t->kind == TokenKind::Comma) {
            return t->precedence;
        }
        return -1;
    }

    void add_fake_parens(std::size_t start, int precedence) {
        if (start >= tokens_.size()) {
            return;
        }
        tokens_[start].fake_lparens.push_back(precedence);
        if (precedence > kPrecUnknown) {
            tokens_[start].starts_binary_expression = true;
        }
        // Running off the end means the region was cut at the query line (or
        // the statement is still being typed); leaving the fake paren open is
        // exactly clang-format's mid-line state, so no closer is placed.
        if (pos_ < tokens_.size()) {
            ++tokens_[pos_ > 0 ? pos_ - 1 : 0].fake_rparens;
        }
    }

    void parse(int precedence) {
        while (cur() && cur()->kind == TokenKind::ReturnKw) {
            next();
        }
        if (!cur() || precedence > kPrecArrowAndPeriod) {
            return;
        }
        if (precedence == kPrecConditional) {
            parse_conditional();
            return;
        }
        if (precedence == kPrecUnary) {
            parse_unary();
            return;
        }

        const std::size_t start = pos_;
        SimToken* latest_operator = nullptr;
        int operator_index = 0;

        while (cur()) {
            parse(precedence + 1);

            const int current = current_precedence();
            if (!cur() || cur()->scope == SimToken::Scope::Close ||
                (current != -1 && current < precedence) ||
                (current == kPrecConditional && precedence == kPrecAssignment &&
                 cur()->ternary_colon)) {
                break;
            }

            if (cur()->scope == SimToken::Scope::Open) {
                // Consume the whole bracketed scope, assigning fakes inside.
                while (cur() && cur()->scope != SimToken::Scope::Close) {
                    next();
                    parse(0);
                }
                next();
            } else {
                if (current == precedence) {
                    if (latest_operator) {
                        latest_operator->next_operator = true;
                    }
                    latest_operator = cur();
                    cur()->operator_index = operator_index++;
                }
                next();
            }
        }

        if (latest_operator && (cur() || precedence > 0)) {
            add_fake_parens(start, precedence == kPrecArrowAndPeriod ? kPrecUnknown
                                                                     : precedence);
        }
    }

    void parse_conditional() {
        const std::size_t start = pos_;
        parse(kPrecLogicalOr);
        if (!cur() || !cur()->ternary_question) {
            return;
        }
        next();
        parse(kPrecAssignment);
        if (!cur() || !cur()->ternary_colon) {
            return;
        }
        next();
        parse(kPrecAssignment);
        add_fake_parens(start, kPrecConditional);
    }

    void parse_unary() {
        std::vector<std::size_t> unaries;
        while (cur() && cur()->unary) {
            unaries.push_back(pos_);
            next();
        }
        parse(kPrecArrowAndPeriod);
        for (auto it = unaries.rbegin(); it != unaries.rend(); ++it) {
            add_fake_parens(*it, kPrecUnknown);
        }
    }
};

// ---- the column state machine (ContinuationIndenter port) ------------------

struct SimParen {
    int indent = 0;
    int last_space = 0;
    int nested_block_indent = 0;
    int question_column = 0;
    int first_lessless = 0;
    int call_continuation = 0;
    int variable_pos = 0;
    int start_of_function_call = 0;
    bool is_wrapped_conditional = false;
};

class Replayer {
public:
    Replayer(const std::vector<SimToken>& tokens, int first_indent,
             const CppIndentStyle& style)
        : tokens_(tokens), first_indent_(first_indent), style_(style) {
        stack_.push_back({first_indent, first_indent, first_indent, 0, 0, 0, 0, 0, false});
    }

    // Processes tokens [0, count), then answers getNewLineColumn for `query`
    // (which may be a synthetic token for empty new lines).
    int run(std::size_t count, const SimToken& query) {
        const SimToken* prev_nc = nullptr; // previous non-comment token
        for (std::size_t i = 0; i < count; ++i) {
            const SimToken* prev = i > 0 ? &tokens_[i - 1] : nullptr;
            const SimToken* prev2 = i > 1 ? &tokens_[i - 2] : nullptr;
            step(tokens_[i], prev, prev2, prev_nc,
                 i + 1 < count ? &tokens_[i + 1] : nullptr);
            if (tokens_[i].kind != TokenKind::LineComment &&
                tokens_[i].kind != TokenKind::BlockComment) {
                prev_nc = &tokens_[i];
            }
        }
        return new_line_column(query, prev_nc);
    }

private:
    const std::vector<SimToken>& tokens_;
    const int first_indent_;
    const CppIndentStyle& style_;
    std::vector<SimParen> stack_;
    int start_of_string_literal_ = 0;
    int region_depth_ = 0;

    SimParen& top() { return stack_.back(); }
    const SimParen& top() const { return stack_.back(); }

    void step(const SimToken& t, const SimToken* prev, const SimToken* prev2,
              const SimToken* prev_nc, const SimToken* next) {
        const int col = t.column;

        if (t.newline_before) {
            // addTokenOnNewLine: the actual column stands in for the computed
            // one; state updates mirror ContinuationIndenter.cpp:1160-1252.
            top().nested_block_indent = col;
            if (t.member_access && top().call_continuation == 0) {
                top().call_continuation = col;
            }
            top().last_space = col;
            if (t.lessless) {
                top().last_space += 3; // width of "<< "
            }
        } else if (prev) {
            // addTokenOnCurrentLine (751-1095), same-line state updates.
            if (t.kind == TokenKind::Equals && region_depth_ == 0 &&
                top().variable_pos == 0 && prev != nullptr) {
                top().variable_pos = variable_start_column(prev, prev2);
            }
            if (style_.align_open_bracket && prev->scope == SimToken::Scope::Open &&
                !prev->block) {
                top().indent = col; // AlignAfterOpenBracket (976)
            }
            if (prev->scope == SimToken::Scope::Open && prev2 &&
                (prev2->kind == TokenKind::IfKw || prev2->kind == TokenKind::ForKw)) {
                // The condition of an if/for indents like a second parameter.
                top().last_space = col;
                top().nested_block_indent = col;
            } else if (prev->kind == TokenKind::Comma) {
                top().last_space = col;
            } else if (prev->ternary_question || prev->ternary_colon ||
                       prev->ctor_colon) {
                top().last_space = col;
            } else if (prev->binary &&
                       ((prev->precedence != kPrecAssignment &&
                         (!prev->lessless || prev->operator_index != 0 ||
                          prev->next_operator)) ||
                        t.starts_binary_expression)) {
                // Indent relative to the RHS of the expression
                // (BreakBeforeBinaryOperators: None).
                top().last_space = col;
            }
        }

        // moveStateToNextToken common updates (1700-1801).
        if (t.lessless) {
            if (top().first_lessless == 0) {
                top().first_lessless = col;
            }
        }
        if (style_.break_before_ternary && t.ternary_question) {
            top().question_column = col;
        }
        if (!style_.break_before_ternary && prev && prev->ternary_question &&
            !t.ternary_colon) {
            top().question_column = col;
        }
        if (t.ternary_question &&
            (t.newline_before || (next && next->newline_before))) {
            top().is_wrapped_conditional = true;
        }
        if (t.member_access) {
            top().start_of_function_call = t.next_operator ? col : 0;
        }
        if (t.ctor_colon) {
            // BeforeColon: items align after ": "; BeforeComma: the ','
            // lands exactly under the ':'.
            const bool comma_prepended =
                style_.constructor_initializers ==
                CppIndentStyle::ConstructorInitializerStyle::AlignWithColon;
            top().indent = col + (comma_prepended ? 0 : 2);
            top().nested_block_indent = top().indent;
        }
        if ((t.binary || t.ternary_question || t.ternary_colon) && t.newline_before) {
            top().nested_block_indent = col + t.length + 1;
        }

        move_past_fake_lparens(t, prev_nc);
        if (t.scope == SimToken::Scope::Close) {
            for (int i = 0; i < t.close_count; ++i) {
                if (stack_.size() > 1) {
                    stack_.pop_back();
                }
                --region_depth_;
            }
        }
        if (t.scope == SimToken::Scope::Open) {
            push_scope(t);
            ++region_depth_;
        }
        for (int i = 0; i < t.fake_rparens && stack_.size() > 1; ++i) {
            const int variable_pos = top().variable_pos;
            stack_.pop_back();
            top().variable_pos = variable_pos;
        }

        // String-literal run tracking (1845): identifiers and comments keep
        // the run alive ("a" PRIu32 "b"), everything else resets it.
        if (is_string(t.kind)) {
            if (start_of_string_literal_ == 0) {
                start_of_string_literal_ = col;
            }
        } else if (t.kind != TokenKind::Identifier &&
                   t.kind != TokenKind::PreprocessorHash &&
                   t.kind != TokenKind::LineComment &&
                   t.kind != TokenKind::BlockComment) {
            start_of_string_literal_ = 0;
        }
    }

    // clang-format walks back over the tokens bound to the variable name
    // (e.g. '*') to find where the declarator starts (802-815).
    int variable_start_column(const SimToken* prev, const SimToken* prev2) {
        int column = prev ? prev->column : 0;
        if (prev && prev2 &&
            (prev2->kind == TokenKind::Star || prev2->kind == TokenKind::Amp) &&
            prev2->column + prev2->length == column) {
            column = prev2->column;
        }
        return column;
    }

    void move_past_fake_lparens(const SimToken& t, const SimToken* prev) {
        if (t.fake_lparens.empty()) {
            return;
        }
        // 1892: no extra indent for the first fake paren after return,
        // assignments, or opening brackets.
        bool skip_first_extra_indent =
            prev && (prev->scope == SimToken::Scope::Open ||
                     prev->kind == TokenKind::Semicolon ||
                     prev->kind == TokenKind::ReturnKw ||
                     (prev->binary && prev->precedence == kPrecAssignment &&
                      style_.align_operands));
        for (auto it = t.fake_lparens.rbegin(); it != t.fake_lparens.rend(); ++it) {
            const int level = *it;
            SimParen state = top(); // fake parens inherit the current state
            state.is_wrapped_conditional = false;

            const bool align = style_.align_operands || level < kPrecAssignment;
            const bool after_return = prev && prev->kind == TokenKind::ReturnKw &&
                                      level == kPrecUnknown;
            if (align && !after_return &&
                (style_.align_open_bracket || level > kPrecComma ||
                 region_depth_ == 0)) {
                state.indent =
                    std::max({t.column, state.indent, top().last_space});
            }
            if (level > kPrecUnknown) {
                state.last_space = std::max(state.last_space, t.column);
            }
            if (level != kPrecConditional && !t.unary && style_.align_open_bracket) {
                state.start_of_function_call = t.column;
            }
            const bool chained_conditional =
                level == kPrecConditional && prev && prev->ternary_colon &&
                it + 1 == t.fake_lparens.rend() && !top().is_wrapped_conditional;
            if (!chained_conditional &&
                (level == kPrecConditional ||
                 (!skip_first_extra_indent && level > kPrecAssignment))) {
                state.indent += style_.continuation_indent;
            }
            stack_.push_back(state);
            skip_first_extra_indent = false;
        }
    }

    void push_scope(const SimToken& t) {
        const int cont = style_.continuation_indent;
        SimParen state;
        state.last_space = top().last_space;
        if (t.block) {
            // Opaque child block; only pushed so the matching '}' pops.
            state.indent = top().indent;
        } else if (t.braced_init) {
            const int width =
                style_.brace_init_continuation ? cont : style_.indent_width;
            state.indent = top().last_space + width;
            state.nested_block_indent =
                std::max(top().start_of_function_call, top().nested_block_indent);
            if (t.multi_param) {
                state.nested_block_indent =
                    std::max(state.nested_block_indent, t.column + 1);
            }
        } else {
            state.indent =
                std::max(top().last_space, top().start_of_function_call) + cont;
            state.nested_block_indent =
                std::max(top().start_of_function_call, top().nested_block_indent);
            if (t.kind == TokenKind::Less) {
                // 2085: '<' inside '(' keeps at least the paren's alignment.
                state.indent = std::max(state.indent, top().indent);
                state.last_space = std::max(state.last_space, top().indent);
            }
        }
        stack_.push_back(state);
    }

    // getNewLineColumn (1387-1686), C++ subset in upstream order.
    int new_line_column(const SimToken& t, const SimToken* prev) const {
        const int cont = style_.continuation_indent;
        const int continuation_indent =
            std::max(top().last_space, top().indent) + cont;

        if ((t.kind == TokenKind::RBrace || t.kind == TokenKind::RBracket) &&
            stack_.size() > 1) {
            return stack_[stack_.size() - 2].last_space; // closes a braced init
        }
        if (t.kind == TokenKind::RParen && stack_.size() > 1) {
            return stack_[stack_.size() - 2].last_space;
        }
        if (is_string(t.kind) && start_of_string_literal_ != 0) {
            return start_of_string_literal_;
        }
        if (t.lessless && top().first_lessless != 0) {
            return top().first_lessless;
        }
        if (t.member_access) {
            return top().call_continuation == 0 ? continuation_indent
                                                : top().call_continuation;
        }
        if (top().question_column != 0 &&
            ((t.ternary_colon) ||
             (prev && (prev->ternary_question || prev->ternary_colon)))) {
            const bool chained =
                ((t.ternary_colon && !t.fake_lparens.empty() &&
                  t.fake_lparens.back() == kPrecConditional) ||
                 (prev && prev->ternary_colon && !t.fake_lparens.empty() &&
                  t.fake_lparens.back() == kPrecConditional)) &&
                !top().is_wrapped_conditional;
            if (chained) {
                int indent = top().indent;
                if (style_.align_operands) {
                    indent -= cont;
                }
                return indent;
            }
            return top().question_column;
        }
        if (prev && prev->kind == TokenKind::Comma && top().variable_pos != 0) {
            return top().variable_pos;
        }
        if (prev && (prev->kind == TokenKind::ColonColon ||
                     prev->kind == TokenKind::Equals)) {
            return continuation_indent;
        }
        if (prev && prev->kind == TokenKind::RParen && !t.binary &&
            t.kind != TokenKind::Colon) {
            return continuation_indent;
        }
        if (top().indent == first_indent_ && prev &&
            prev->kind != TokenKind::RBrace) {
            return top().indent + cont;
        }
        return top().indent;
    }
};

} // namespace

std::optional<int> expression_continuation_column(const DocumentSnapshot& snapshot,
                                                  const SyntaxTree& tree, std::uint32_t line,
                                                  SyntaxNodeId controlling,
                                                  const CppIndentStyle& style,
                                                  std::vector<std::string>& trace) {
    const Text& lines = snapshot.content();
    const TextCharSource text(snapshot.content());
    const std::vector<Token>& tokens = tree.tokens();

    // Region: climb to the node whose parent is a block-level scope — the
    // clang-format "unwrapped line" approximation.
    auto is_block_scope = [](SyntaxKind k) {
        return k == SyntaxKind::TranslationUnit || k == SyntaxKind::CompoundStatement ||
               k == SyntaxKind::NamespaceBody || k == SyntaxKind::ClassBody ||
               k == SyntaxKind::CaseSection;
    };
    SyntaxNodeId region = controlling;
    while (tree.node(region).parent != kInvalidNode &&
           !is_block_scope(tree.node(tree.node(region).parent).kind)) {
        region = tree.node(region).parent;
        if (tree.node(region).kind == SyntaxKind::PreprocessorDirective) {
            return std::nullopt; // macro bodies keep the T2 rules
        }
    }
    const SyntaxNode& region_node = tree.node(region);
    constexpr std::uint32_t kMaxRegionTokens = 4096;
    if (region_node.end_token - region_node.first_token > kMaxRegionTokens) {
        return std::nullopt;
    }

    const TextOffset line_start = lines.line_start(line);

    // The parser may split one logical statement into sibling nodes
    // (`static const` + the struct-typed declarator). clang-format's
    // unwrapped line starts at the line's first token, so extend backwards
    // across same-line siblings, stopping at statement boundary tokens.
    std::uint32_t first_token = tree.node(region).first_token;
    {
        const std::uint32_t region_line =
            lines.position(tokens[first_token].range.start).line;
        std::uint32_t i = first_token;
        while (i > 0) {
            const Token& prev_tok = tokens[i - 1];
            if (is_trivia(prev_tok.kind)) {
                --i;
                continue;
            }
            if (lines.position(prev_tok.range.start).line != region_line ||
                prev_tok.kind == TokenKind::Semicolon ||
                prev_tok.kind == TokenKind::Colon ||
                prev_tok.kind == TokenKind::LBrace ||
                prev_tok.kind == TokenKind::RBrace) {
                break;
            }
            --i;
            first_token = i;
        }
    }

    // Collect the tokens covered by descendant code blocks so their interiors
    // can be skipped (they are separate lines in clang-format's model).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> block_interiors;
    std::vector<std::uint32_t> block_bounds;
    {
        // Outermost descendant code blocks of the region, in token order.
        std::vector<SyntaxNodeId> pending(tree.node(region).children.rbegin(),
                                          tree.node(region).children.rend());
        while (!pending.empty()) {
            const SyntaxNodeId id = pending.back();
            pending.pop_back();
            const SyntaxNode& n = tree.node(id);
            if (n.kind == SyntaxKind::CompoundStatement && n.first_token < n.end_token) {
                block_interiors.emplace_back(n.first_token + 1, n.end_token - 1);
                block_bounds.push_back(n.first_token);
                block_bounds.push_back(n.end_token - 1);
                continue; // nested blocks live inside the skipped range
            }
            pending.insert(pending.end(), n.children.rbegin(), n.children.rend());
        }
        std::ranges::sort(block_interiors);
        std::ranges::sort(block_bounds);
    }
    auto inside_block = [&](std::uint32_t index) {
        for (auto [begin, end] : block_interiors) {
            if (index >= begin && index < end) {
                return true;
            }
        }
        return false;
    };
    auto is_block_bound = [&](std::uint32_t index) {
        return std::ranges::binary_search(block_bounds, index);
    };

    auto display_column = [&](TextOffset offset) {
        const std::uint32_t start = lines.line_start(lines.position(offset).line).value;
        int col = 0;
        for (std::uint32_t i = start; i < offset.value; ++i) {
            col += text[i] == '\t' ? style.tab_width - col % style.tab_width : 1;
        }
        return col;
    };

    // Build the simulation token list, stopping at the query line.
    std::vector<SimToken> sim;
    // First sim index at/after the query line: the replay stops here. The
    // fake-paren assignment runs over the whole region — the code after the
    // query line exists in the file, exactly like clang-format annotating the
    // full unwrapped line before formatting it token by token.
    std::optional<std::size_t> replay_count;
    std::optional<std::size_t> query_index;
    std::uint32_t prev_line = lines.position(tokens[first_token].range.start).line;
    std::vector<std::size_t> question_stack;
    bool after_operator_kw = false;
    for (std::uint32_t i = first_token; i < region_node.end_token; ++i) {
        const Token& tok = tokens[i];
        const bool comment =
            tok.kind == TokenKind::LineComment || tok.kind == TokenKind::BlockComment;
        if ((is_trivia(tok.kind) && !comment) || tok.kind == TokenKind::EndOfFile ||
            tok.range.length() == 0) {
            continue;
        }
        if (inside_block(i)) {
            continue;
        }
        const std::uint32_t tok_line = lines.position(tok.range.start).line;
        if (comment && tok_line == prev_line) {
            continue; // trailing comments never carry alignment state
        }
        const bool at_query = tok.range.start.value >= line_start.value;
        if (at_query && !replay_count) {
            replay_count = sim.size();
        }
        if (at_query && !query_index && tok_line == line && !comment) {
            query_index = sim.size(); // rules dispatch on the first code token
        }
        SimToken s;
        s.kind = tok.kind;
        s.length = static_cast<int>(tok.range.length());
        s.column = display_column(tok.range.start);
        s.newline_before = tok_line != prev_line;
        prev_line = tok_line;

        // Scope structure from the CST: a group's first/last tokens.
        const SyntaxNodeId deepest = tree.node_at(tok.range.start);
        const SyntaxNode& dn = tree.node(deepest);
        const bool group = dn.kind == SyntaxKind::ParenGroup ||
                           dn.kind == SyntaxKind::BracketGroup ||
                           dn.kind == SyntaxKind::BraceGroup ||
                           dn.kind == SyntaxKind::TemplateArgumentList;
        if (is_block_bound(i)) {
            s.scope = tokens[i].kind == TokenKind::LBrace ? SimToken::Scope::Open
                                                          : SimToken::Scope::Close;
            s.block = true;
        } else if (group && i == dn.first_token) {
            s.scope = SimToken::Scope::Open;
            s.braced_init = dn.kind == SyntaxKind::BraceGroup;
            int commas = 0;
            for (std::uint32_t j = dn.first_token + 1; j + 1 < dn.end_token; ++j) {
                if (tokens[j].kind == TokenKind::Comma &&
                    tree.node_at(tokens[j].range.start) == deepest) {
                    ++commas;
                }
            }
            s.multi_param = commas >= 1;
        } else if (group && i + 1 == dn.end_token && tok.kind != TokenKind::Comma &&
                   tok.kind != TokenKind::Semicolon &&
                   tok.kind != TokenKind::Identifier) {
            // The group's last token closes it whatever it lexed as ('>>'
            // closes two template groups with one token).
            s.scope = SimToken::Scope::Close;
            s.braced_init = dn.kind == SyntaxKind::BraceGroup;
            s.close_count = 1;
            for (SyntaxNodeId up = dn.parent; up != kInvalidNode;
                 up = tree.node(up).parent) {
                const SyntaxNode& un = tree.node(up);
                const bool up_group = un.kind == SyntaxKind::ParenGroup ||
                                      un.kind == SyntaxKind::BracketGroup ||
                                      un.kind == SyntaxKind::BraceGroup ||
                                      un.kind == SyntaxKind::TemplateArgumentList;
                if (up_group && i + 1 == un.end_token) {
                    ++s.close_count;
                } else {
                    break;
                }
            }
        }

        // Operator classification. Format-clean input always spaces binary
        // operators, while template '<' and pointer '*'/'&' hug an operand —
        // adjacency disambiguates what TokenAnnotator infers from context.
        const bool space_before =
            tok.range.start.value > 0 &&
            (text[tok.range.start.value - 1] == ' ' ||
             text[tok.range.start.value - 1] == '\t' ||
             text[tok.range.start.value - 1] == '\n');
        const bool space_after = tok.range.end.value < text.size() &&
                                 (text[tok.range.end.value] == ' ' ||
                                  text[tok.range.end.value] == '\t' ||
                                  text[tok.range.end.value] == '\n');
        const SimToken* prev_sig = sim.empty() ? nullptr : &sim.back();
        if (s.scope == SimToken::Scope::None && !after_operator_kw) {
            if (tok.kind == TokenKind::Colon) {
                if (dn.kind == SyntaxKind::CtorInitializerList ||
                    (dn.parent != kInvalidNode &&
                     tree.node(dn.parent).kind == SyntaxKind::CtorInitializerList)) {
                    s.ctor_colon = true;
                } else if (!question_stack.empty()) {
                    s.ternary_colon = true;
                    question_stack.pop_back();
                }
            } else if (tok.kind == TokenKind::Question) {
                // A lone '?' in an expression region is a ternary even before
                // its ':' exists (the mid-typing case).
                s.ternary_question = true;
                question_stack.push_back(sim.size());
            } else if (tok.kind == TokenKind::Comma) {
                s.precedence = kPrecComma;
            } else {
                const int prec = binary_precedence(s);
                // Pointer '*'/'&'/'&&' and template '<'/'>' are only binary
                // operators when spaced on both sides.
                const bool ambiguous = s.kind == TokenKind::Star || s.kind == TokenKind::Amp ||
                                       s.kind == TokenKind::AmpAmp || s.kind == TokenKind::Less ||
                                       s.kind == TokenKind::Greater;
                const bool operand_before = prev_sig && is_operand_end(*prev_sig);
                if (s.kind == TokenKind::Arrow || s.kind == TokenKind::Period ||
                    s.kind == TokenKind::ArrowStar || s.kind == TokenKind::PeriodStar) {
                    if (operand_before) {
                        s.member_access = true;
                    }
                } else if (prec == kPrecAssignment) {
                    s.binary = true;
                    s.precedence = kPrecAssignment;
                } else if (prec > 0) {
                    if (operand_before && (!ambiguous || (space_before && space_after))) {
                        s.binary = true;
                        s.precedence = prec;
                        s.lessless = s.kind == TokenKind::LessLess;
                    } else if (!operand_before) {
                        s.unary = is_unary_spelling(s);
                    }
                } else if (is_unary_spelling(s) && !operand_before) {
                    s.unary = true;
                }
            }
        }
        after_operator_kw = tok.kind == TokenKind::OperatorKw;
        sim.push_back(std::move(s));
    }

    if (!replay_count) {
        replay_count = sim.size();
    }
    if (*replay_count == 0) {
        return std::nullopt;
    }

    FakeParenAssigner(sim).run();

    SimToken synthetic;
    if (!query_index) {
        // Empty new line (Enter): synthesize a plain identifier query.
        synthetic.kind = TokenKind::Identifier;
        synthetic.newline_before = true;
    }
    const SimToken& query = query_index ? sim[*query_index] : synthetic;

    const std::uint32_t region_line =
        lines.position(tokens[first_token].range.start).line;
    int first_indent = 0;
    for (std::uint32_t i = lines.line_start(region_line).value; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ') {
            ++first_indent;
        } else if (c == '\t') {
            first_indent += style.tab_width - first_indent % style.tab_width;
        } else {
            break;
        }
    }

    Replayer replayer(sim, first_indent, style);
    const int column = replayer.run(*replay_count, query);
    trace.push_back(std::format(
        "expression continuation engine: region {} at line {}, column {}",
        syntax_kind_name(region_node.kind), region_line + 1, column));
    return column;
}

} // namespace cind
