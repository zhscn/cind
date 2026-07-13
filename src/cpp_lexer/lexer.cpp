#include "cpp_lexer/lexer.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace cind {

std::string_view token_kind_name(TokenKind kind) {
    switch (kind) {
    case TokenKind::Whitespace: return "Whitespace";
    case TokenKind::Newline: return "Newline";
    case TokenKind::LineComment: return "LineComment";
    case TokenKind::BlockComment: return "BlockComment";
    case TokenKind::Identifier: return "Identifier";
    case TokenKind::Number: return "Number";
    case TokenKind::StringLiteral: return "StringLiteral";
    case TokenKind::RawStringLiteral: return "RawStringLiteral";
    case TokenKind::CharacterLiteral: return "CharacterLiteral";
    case TokenKind::NamespaceKw: return "NamespaceKw";
    case TokenKind::ClassKw: return "ClassKw";
    case TokenKind::StructKw: return "StructKw";
    case TokenKind::EnumKw: return "EnumKw";
    case TokenKind::UnionKw: return "UnionKw";
    case TokenKind::SwitchKw: return "SwitchKw";
    case TokenKind::CaseKw: return "CaseKw";
    case TokenKind::DefaultKw: return "DefaultKw";
    case TokenKind::PublicKw: return "PublicKw";
    case TokenKind::ProtectedKw: return "ProtectedKw";
    case TokenKind::PrivateKw: return "PrivateKw";
    case TokenKind::IfKw: return "IfKw";
    case TokenKind::ElseKw: return "ElseKw";
    case TokenKind::ForKw: return "ForKw";
    case TokenKind::WhileKw: return "WhileKw";
    case TokenKind::DoKw: return "DoKw";
    case TokenKind::ReturnKw: return "ReturnKw";
    case TokenKind::TemplateKw: return "TemplateKw";
    case TokenKind::OperatorKw: return "OperatorKw";
    case TokenKind::LBrace: return "LBrace";
    case TokenKind::RBrace: return "RBrace";
    case TokenKind::LParen: return "LParen";
    case TokenKind::RParen: return "RParen";
    case TokenKind::LBracket: return "LBracket";
    case TokenKind::RBracket: return "RBracket";
    case TokenKind::Less: return "Less";
    case TokenKind::Greater: return "Greater";
    case TokenKind::Colon: return "Colon";
    case TokenKind::ColonColon: return "ColonColon";
    case TokenKind::Comma: return "Comma";
    case TokenKind::Semicolon: return "Semicolon";
    case TokenKind::Arrow: return "Arrow";
    case TokenKind::Equals: return "Equals";
    case TokenKind::Punctuator: return "Punctuator";
    case TokenKind::PreprocessorHash: return "PreprocessorHash";
    case TokenKind::Invalid: return "Invalid";
    case TokenKind::EndOfFile: return "EndOfFile";
    }
    return "?";
}

namespace {

bool is_ident_start(unsigned char c) { return std::isalpha(c) != 0 || c == '_' || c >= 0x80; }
bool is_ident_continue(unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c >= 0x80; }
bool is_horizontal_space(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

TokenKind keyword_kind(std::string_view ident) {
    static const std::unordered_map<std::string_view, TokenKind> kKeywords = {
        {"namespace", TokenKind::NamespaceKw}, {"class", TokenKind::ClassKw},
        {"struct", TokenKind::StructKw},       {"enum", TokenKind::EnumKw},
        {"union", TokenKind::UnionKw},         {"switch", TokenKind::SwitchKw},
        {"case", TokenKind::CaseKw},           {"default", TokenKind::DefaultKw},
        {"public", TokenKind::PublicKw},       {"protected", TokenKind::ProtectedKw},
        {"private", TokenKind::PrivateKw},     {"if", TokenKind::IfKw},
        {"else", TokenKind::ElseKw},           {"for", TokenKind::ForKw},
        {"while", TokenKind::WhileKw},         {"do", TokenKind::DoKw},
        {"return", TokenKind::ReturnKw},       {"template", TokenKind::TemplateKw},
        {"operator", TokenKind::OperatorKw},
    };
    auto it = kKeywords.find(ident);
    return it == kKeywords.end() ? TokenKind::Identifier : it->second;
}

bool is_encoding_prefix(std::string_view s) {
    return s == "u8" || s == "u" || s == "U" || s == "L";
}

std::string extract_raw_delimiter(std::string_view token_text) {
    std::size_t quote = token_text.find('"');
    std::size_t paren = token_text.find('(', quote);
    return std::string(token_text.substr(quote + 1, paren - quote - 1));
}

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    LexOutput run() {
        while (pos_ < text_.size()) {
            lex_one();
        }
        emit(TokenKind::EndOfFile, pos_);
        finalize_line_states();
        return std::move(out_);
    }

private:
    char peek(std::size_t ahead = 1) const {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }
    bool starts_with(std::string_view s) const { return text_.substr(pos_).starts_with(s); }

    void emit(TokenKind kind, std::size_t start, LexicalFlags flags = LexicalFlags::None) {
        if (in_pp_line_) {
            flags |= LexicalFlags::PreprocessorLine;
        }
        if (!is_trivia(kind) && kind != TokenKind::EndOfFile) {
            line_has_content_ = true;
        }
        out_.tokens.push_back(Token{
            kind, make_range(static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(pos_)),
            flags});
    }

    void lex_one() {
        const std::size_t start = pos_;
        const char c = text_[pos_];

        if (c == '\n') {
            ++pos_;
            emit(TokenKind::Newline, start);
            in_pp_line_ = false;
            line_has_content_ = false;
            return;
        }
        if (is_horizontal_space(c)) {
            while (pos_ < text_.size() && is_horizontal_space(text_[pos_])) {
                ++pos_;
            }
            emit(TokenKind::Whitespace, start);
            return;
        }
        if (c == '\\' && peek() == '\n') {
            pos_ += 2;
            emit(TokenKind::Whitespace, start, LexicalFlags::EscapedNewline);
            return; // a splice does not end a preprocessor line
        }
        if (c == '/' && peek() == '/') {
            pos_ += 2;
            while (pos_ < text_.size() && text_[pos_] != '\n') {
                ++pos_;
            }
            emit(TokenKind::LineComment, start);
            return;
        }
        if (c == '/' && peek() == '*') {
            lex_block_comment(start);
            return;
        }
        if (c == '"') {
            lex_string(start, TokenKind::StringLiteral, '"');
            return;
        }
        if (c == '\'') {
            lex_string(start, TokenKind::CharacterLiteral, '\'');
            return;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(peek())) != 0)) {
            lex_number(start);
            return;
        }
        if (is_ident_start(static_cast<unsigned char>(c))) {
            lex_identifier(start);
            return;
        }
        if (c == '#') {
            lex_hash(start);
            return;
        }
        lex_punctuator_or_invalid(start);
    }

    void lex_block_comment(std::size_t start) {
        pos_ += 2;
        LexicalFlags flags = LexicalFlags::None;
        while (true) {
            if (pos_ + 1 >= text_.size()) {
                pos_ = text_.size();
                flags = LexicalFlags::Unterminated;
                break;
            }
            if (text_[pos_] == '*' && text_[pos_ + 1] == '/') {
                pos_ += 2;
                break;
            }
            ++pos_;
        }
        emit(TokenKind::BlockComment, start, flags);
    }

    // pos_ is at the opening quote; `start` may be earlier (encoding prefix).
    void lex_string(std::size_t start, TokenKind kind, char quote) {
        ++pos_;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == '\n') {
                emit(kind, start, LexicalFlags::Unterminated);
                return; // the newline stays outside; damage is line-local
            }
            if (ch == '\\' && pos_ + 1 < text_.size()) {
                pos_ += 2; // escape, including a splice inside the literal
                continue;
            }
            ++pos_;
            if (ch == quote) {
                emit(kind, start);
                return;
            }
        }
        emit(kind, start, LexicalFlags::Unterminated);
    }

    // pos_ is at the opening quote, after `R` or `u8R` etc.
    void lex_raw_string(std::size_t start) {
        const std::size_t quote = pos_;
        ++pos_;
        const std::size_t delim_start = pos_;
        while (pos_ < text_.size() && pos_ - delim_start < 16) {
            const char ch = text_[pos_];
            if (ch == '(' || ch == ')' || ch == '"' || ch == '\\' || ch == ' ' || ch == '\n') {
                break;
            }
            ++pos_;
        }
        if (pos_ >= text_.size() || text_[pos_] != '(') {
            // Malformed raw string; degrade to a regular string literal.
            pos_ = quote;
            lex_string(start, TokenKind::StringLiteral, '"');
            return;
        }
        const std::string terminator =
            ")" + std::string(text_.substr(delim_start, pos_ - delim_start)) + "\"";
        ++pos_;
        const std::size_t found = text_.find(terminator, pos_);
        if (found == std::string_view::npos) {
            pos_ = text_.size();
            emit(TokenKind::RawStringLiteral, start, LexicalFlags::Unterminated);
            return;
        }
        pos_ = found + terminator.size();
        emit(TokenKind::RawStringLiteral, start);
    }

    // pp-number: deliberately permissive, matches the preprocessing grammar.
    void lex_number(std::size_t start) {
        ++pos_;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            const char prev = text_[pos_ - 1];
            if ((ch == '+' || ch == '-') &&
                (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P')) {
                ++pos_;
                continue;
            }
            if (is_ident_continue(static_cast<unsigned char>(ch)) || ch == '.') {
                ++pos_;
                continue;
            }
            if (ch == '\'' && pos_ + 1 < text_.size() &&
                is_ident_continue(static_cast<unsigned char>(text_[pos_ + 1]))) {
                pos_ += 2;
                continue;
            }
            break;
        }
        emit(TokenKind::Number, start);
    }

    void lex_identifier(std::size_t start) {
        while (pos_ < text_.size() && is_ident_continue(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        const std::string_view ident = text_.substr(start, pos_ - start);
        if (pos_ < text_.size()) {
            const char next = text_[pos_];
            if (next == '"') {
                if (ident == "R" ||
                    (ident.ends_with('R') && is_encoding_prefix(ident.substr(0, ident.size() - 1)))) {
                    lex_raw_string(start);
                    return;
                }
                if (is_encoding_prefix(ident)) {
                    lex_string(start, TokenKind::StringLiteral, '"');
                    return;
                }
            } else if (next == '\'' && is_encoding_prefix(ident)) {
                lex_string(start, TokenKind::CharacterLiteral, '\'');
                return;
            }
        }
        emit(keyword_kind(ident), start);
    }

    void lex_hash(std::size_t start) {
        if (starts_with("##")) {
            pos_ += 2;
            emit(TokenKind::Punctuator, start);
            return;
        }
        ++pos_;
        if (!line_has_content_ && !in_pp_line_) {
            in_pp_line_ = true; // before emit, so the hash itself carries the flag
            emit(TokenKind::PreprocessorHash, start);
        } else {
            emit(TokenKind::Punctuator, start); // stringize operator etc.
        }
    }

    void lex_punctuator_or_invalid(std::size_t start) {
        static constexpr std::string_view kThree[] = {"<<=", ">>=", "<=>", "->*", "..."};
        static constexpr std::string_view kTwo[] = {"::", "<<", ">>", "<=", ">=", "==",
                                                    "!=", "&&", "||", "+=", "-=", "*=",
                                                    "/=", "%=", "&=", "|=", "^=", "->",
                                                    "++", "--", ".*"};
        for (const std::string_view op : kThree) {
            if (starts_with(op)) {
                pos_ += 3;
                emit(TokenKind::Punctuator, start);
                return;
            }
        }
        for (const std::string_view op : kTwo) {
            if (starts_with(op)) {
                pos_ += 2;
                TokenKind kind = op == "::"   ? TokenKind::ColonColon
                                 : op == "->" ? TokenKind::Arrow
                                              : TokenKind::Punctuator;
                emit(kind, start);
                return;
            }
        }
        const char c = text_[pos_++];
        TokenKind kind;
        switch (c) {
        case '{': kind = TokenKind::LBrace; break;
        case '}': kind = TokenKind::RBrace; break;
        case '(': kind = TokenKind::LParen; break;
        case ')': kind = TokenKind::RParen; break;
        case '[': kind = TokenKind::LBracket; break;
        case ']': kind = TokenKind::RBracket; break;
        case '<': kind = TokenKind::Less; break;
        case '>': kind = TokenKind::Greater; break;
        case ':': kind = TokenKind::Colon; break;
        case ',': kind = TokenKind::Comma; break;
        case ';': kind = TokenKind::Semicolon; break;
        case '=': kind = TokenKind::Equals; break;
        case '+':
        case '-':
        case '*':
        case '/':
        case '%':
        case '!':
        case '&':
        case '|':
        case '^':
        case '~':
        case '?':
        case '.': kind = TokenKind::Punctuator; break;
        default: kind = TokenKind::Invalid; break;
        }
        emit(kind, start);
    }

    // Derives the lexer state entering each line from the finished token
    // stream. Every '\n' belongs to exactly one token, so each occurrence
    // appends the state for the following line.
    void finalize_line_states() {
        out_.line_states.emplace_back();
        for (const Token& token : out_.tokens) {
            const std::string_view tok_text =
                text_.substr(token.range.start.value, token.range.length());
            if (tok_text.find('\n') == std::string_view::npos) {
                continue;
            }
            LexerState state;
            if (token.kind == TokenKind::BlockComment) {
                state.inside_block_comment = true;
            } else if (token.kind == TokenKind::RawStringLiteral) {
                state.inside_raw_string = true;
                state.raw_delimiter = extract_raw_delimiter(tok_text);
            } else if (token.kind == TokenKind::Whitespace &&
                       has_flag(token.flags, LexicalFlags::EscapedNewline) &&
                       has_flag(token.flags, LexicalFlags::PreprocessorLine)) {
                state.preprocessor_continuation = true;
            }
            const auto count = std::count(tok_text.begin(), tok_text.end(), '\n');
            for (std::ptrdiff_t i = 0; i < count; ++i) {
                out_.line_states.push_back(state);
            }
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    bool in_pp_line_ = false;
    bool line_has_content_ = false;
    LexOutput out_;
};

} // namespace

LexOutput lex(std::string_view text) { return Lexer(text).run(); }

} // namespace cind
