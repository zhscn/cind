#include "cpp_lexer/lexer.hpp"

#include "document/text.hpp"

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
    case TokenKind::Plus: return "Plus";
    case TokenKind::Minus: return "Minus";
    case TokenKind::Star: return "Star";
    case TokenKind::Slash: return "Slash";
    case TokenKind::Percent: return "Percent";
    case TokenKind::Exclaim: return "Exclaim";
    case TokenKind::Amp: return "Amp";
    case TokenKind::Pipe: return "Pipe";
    case TokenKind::Caret: return "Caret";
    case TokenKind::Tilde: return "Tilde";
    case TokenKind::Question: return "Question";
    case TokenKind::Period: return "Period";
    case TokenKind::Ellipsis: return "Ellipsis";
    case TokenKind::PlusPlus: return "PlusPlus";
    case TokenKind::MinusMinus: return "MinusMinus";
    case TokenKind::AmpAmp: return "AmpAmp";
    case TokenKind::PipePipe: return "PipePipe";
    case TokenKind::EqualEqual: return "EqualEqual";
    case TokenKind::ExclaimEqual: return "ExclaimEqual";
    case TokenKind::LessEqual: return "LessEqual";
    case TokenKind::GreaterEqual: return "GreaterEqual";
    case TokenKind::Spaceship: return "Spaceship";
    case TokenKind::LessLess: return "LessLess";
    case TokenKind::GreaterGreater: return "GreaterGreater";
    case TokenKind::PeriodStar: return "PeriodStar";
    case TokenKind::ArrowStar: return "ArrowStar";
    case TokenKind::PlusEqual: return "PlusEqual";
    case TokenKind::MinusEqual: return "MinusEqual";
    case TokenKind::StarEqual: return "StarEqual";
    case TokenKind::SlashEqual: return "SlashEqual";
    case TokenKind::PercentEqual: return "PercentEqual";
    case TokenKind::AmpEqual: return "AmpEqual";
    case TokenKind::PipeEqual: return "PipeEqual";
    case TokenKind::CaretEqual: return "CaretEqual";
    case TokenKind::LessLessEqual: return "LessLessEqual";
    case TokenKind::GreaterGreaterEqual: return "GreaterGreaterEqual";
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

// Sequential character access over a contiguous string.
struct StringSource {
    std::string_view text;

    std::size_t size() const { return text.size(); }
    char at(std::size_t pos) const { return text[pos]; }
    bool starts_with(std::size_t pos, std::string_view s) const {
        return text.substr(pos).starts_with(s);
    }
    void extract(std::size_t pos, std::size_t len, std::string& out) const {
        out.assign(text.substr(pos, len));
    }
};

// Sequential character access over a chunked Text value. Keeps a window on
// the current chunk; leaving the window re-seeks in O(log n), which a
// forward scan does once per chunk.
class TextSource {
public:
    explicit TextSource(const Text& text) : text_(text), size_(text.size_bytes()) {}

    std::size_t size() const { return size_; }
    char at(std::size_t pos) const {
        if (pos < window_start_ || pos >= window_end_) {
            refill(pos);
        }
        return window_[pos - window_start_];
    }
    bool starts_with(std::size_t pos, std::string_view s) const {
        if (pos + s.size() > size_) {
            return false;
        }
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (at(pos + i) != s[i]) {
                return false;
            }
        }
        return true;
    }
    void extract(std::size_t pos, std::size_t len, std::string& out) const {
        out.clear();
        for (std::size_t i = 0; i < len; ++i) {
            out.push_back(at(pos + i));
        }
    }

private:
    void refill(std::size_t pos) const {
        TextCursor cursor(text_, TextOffset{static_cast<std::uint32_t>(pos)});
        window_ = cursor.chunk();
        window_start_ = pos;
        window_end_ = pos + window_.size();
    }

    const Text& text_;
    std::size_t size_;
    mutable std::string_view window_;
    mutable std::size_t window_start_ = 0;
    mutable std::size_t window_end_ = 0;
};

template <typename Source>
class Lexer {
public:
    explicit Lexer(const Source& source) : src_(source), size_(source.size()) {}

    LexOutput run() {
        out_.line_states.emplace_back();
        while (pos_ < size_) {
            lex_one();
        }
        emit(TokenKind::EndOfFile, pos_);
        return std::move(out_);
    }

private:
    char at(std::size_t pos) const { return src_.at(pos); }
    char peek(std::size_t ahead = 1) const {
        return pos_ + ahead < size_ ? src_.at(pos_ + ahead) : '\0';
    }
    bool starts_with(std::string_view s) const { return src_.starts_with(pos_, s); }

    // Cross-line lexical state is recorded while scanning: every consumed
    // '\n' appends the state entering the following line.
    void push_line_state(LexerState state = {}) {
        out_.line_states.push_back(std::move(state));
    }

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
        const char c = at(pos_);

        if (c == '\n') {
            ++pos_;
            emit(TokenKind::Newline, start);
            push_line_state();
            in_pp_line_ = false;
            line_has_content_ = false;
            return;
        }
        if (is_horizontal_space(c)) {
            while (pos_ < size_ && is_horizontal_space(at(pos_))) {
                ++pos_;
            }
            emit(TokenKind::Whitespace, start);
            return;
        }
        if (c == '\\' && peek() == '\n') {
            pos_ += 2;
            LexerState state;
            state.preprocessor_continuation = in_pp_line_;
            push_line_state(std::move(state));
            emit(TokenKind::Whitespace, start, LexicalFlags::EscapedNewline);
            return; // a splice does not end a preprocessor line
        }
        if (c == '/' && peek() == '/') {
            pos_ += 2;
            while (pos_ < size_ && at(pos_) != '\n') {
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
        LexerState state;
        state.inside_block_comment = true;
        while (true) {
            if (pos_ + 1 >= size_) {
                while (pos_ < size_) { // at most the final byte
                    if (at(pos_) == '\n') {
                        push_line_state(state);
                    }
                    ++pos_;
                }
                flags = LexicalFlags::Unterminated;
                break;
            }
            if (at(pos_) == '*' && at(pos_ + 1) == '/') {
                pos_ += 2;
                break;
            }
            if (at(pos_) == '\n') {
                push_line_state(state);
            }
            ++pos_;
        }
        emit(TokenKind::BlockComment, start, flags);
    }

    // pos_ is at the opening quote; `start` may be earlier (encoding prefix).
    void lex_string(std::size_t start, TokenKind kind, char quote) {
        ++pos_;
        while (pos_ < size_) {
            const char ch = at(pos_);
            if (ch == '\n') {
                emit(kind, start, LexicalFlags::Unterminated);
                return; // the newline stays outside; damage is line-local
            }
            if (ch == '\\' && pos_ + 1 < size_) {
                if (at(pos_ + 1) == '\n') {
                    push_line_state(); // splice inside a literal: plain next line
                }
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
        while (pos_ < size_ && pos_ - delim_start < 16) {
            const char ch = at(pos_);
            if (ch == '(' || ch == ')' || ch == '"' || ch == '\\' || ch == ' ' || ch == '\n') {
                break;
            }
            ++pos_;
        }
        if (pos_ >= size_ || at(pos_) != '(') {
            // Malformed raw string; degrade to a regular string literal.
            pos_ = quote;
            lex_string(start, TokenKind::StringLiteral, '"');
            return;
        }
        std::string delimiter;
        src_.extract(delim_start, pos_ - delim_start, delimiter);
        const std::string terminator = ")" + delimiter + "\"";
        ++pos_;
        LexerState state;
        state.inside_raw_string = true;
        state.raw_delimiter = delimiter;
        while (pos_ < size_) {
            const char ch = at(pos_);
            if (ch == ')' && src_.starts_with(pos_, terminator)) {
                pos_ += terminator.size();
                emit(TokenKind::RawStringLiteral, start);
                return;
            }
            if (ch == '\n') {
                push_line_state(state);
            }
            ++pos_;
        }
        emit(TokenKind::RawStringLiteral, start, LexicalFlags::Unterminated);
    }

    // pp-number: deliberately permissive, matches the preprocessing grammar.
    void lex_number(std::size_t start) {
        ++pos_;
        while (pos_ < size_) {
            const char ch = at(pos_);
            const char prev = at(pos_ - 1);
            if ((ch == '+' || ch == '-') &&
                (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P')) {
                ++pos_;
                continue;
            }
            if (is_ident_continue(static_cast<unsigned char>(ch)) || ch == '.') {
                ++pos_;
                continue;
            }
            if (ch == '\'' && pos_ + 1 < size_ &&
                is_ident_continue(static_cast<unsigned char>(at(pos_ + 1)))) {
                pos_ += 2;
                continue;
            }
            break;
        }
        emit(TokenKind::Number, start);
    }

    void lex_identifier(std::size_t start) {
        while (pos_ < size_ && is_ident_continue(static_cast<unsigned char>(at(pos_)))) {
            ++pos_;
        }
        // Keywords and encoding prefixes are all short; longer identifiers
        // need no text at all.
        constexpr std::size_t kMaxInterestingIdent = 9; // "namespace"
        TokenKind kind = TokenKind::Identifier;
        if (pos_ - start <= kMaxInterestingIdent) {
            src_.extract(start, pos_ - start, ident_buffer_);
            const std::string_view ident = ident_buffer_;
            if (pos_ < size_) {
                const char next = at(pos_);
                if (next == '"') {
                    if (ident == "R" ||
                        (ident.ends_with('R') &&
                         is_encoding_prefix(ident.substr(0, ident.size() - 1)))) {
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
            kind = keyword_kind(ident);
        }
        emit(kind, start);
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
        struct Spelling {
            std::string_view text;
            TokenKind kind;
        };
        static constexpr Spelling kThree[] = {
            {"<<=", TokenKind::LessLessEqual}, {">>=", TokenKind::GreaterGreaterEqual},
            {"<=>", TokenKind::Spaceship},     {"->*", TokenKind::ArrowStar},
            {"...", TokenKind::Ellipsis},
        };
        static constexpr Spelling kTwo[] = {
            {"::", TokenKind::ColonColon},   {"<<", TokenKind::LessLess},
            {">>", TokenKind::GreaterGreater}, {"<=", TokenKind::LessEqual},
            {">=", TokenKind::GreaterEqual}, {"==", TokenKind::EqualEqual},
            {"!=", TokenKind::ExclaimEqual}, {"&&", TokenKind::AmpAmp},
            {"||", TokenKind::PipePipe},     {"+=", TokenKind::PlusEqual},
            {"-=", TokenKind::MinusEqual},   {"*=", TokenKind::StarEqual},
            {"/=", TokenKind::SlashEqual},   {"%=", TokenKind::PercentEqual},
            {"&=", TokenKind::AmpEqual},     {"|=", TokenKind::PipeEqual},
            {"^=", TokenKind::CaretEqual},   {"->", TokenKind::Arrow},
            {"++", TokenKind::PlusPlus},     {"--", TokenKind::MinusMinus},
            {".*", TokenKind::PeriodStar},
        };
        for (const Spelling& op : kThree) {
            if (starts_with(op.text)) {
                pos_ += 3;
                emit(op.kind, start);
                return;
            }
        }
        for (const Spelling& op : kTwo) {
            if (starts_with(op.text)) {
                pos_ += 2;
                emit(op.kind, start);
                return;
            }
        }
        const char c = at(pos_++);
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
        case '+': kind = TokenKind::Plus; break;
        case '-': kind = TokenKind::Minus; break;
        case '*': kind = TokenKind::Star; break;
        case '/': kind = TokenKind::Slash; break;
        case '%': kind = TokenKind::Percent; break;
        case '!': kind = TokenKind::Exclaim; break;
        case '&': kind = TokenKind::Amp; break;
        case '|': kind = TokenKind::Pipe; break;
        case '^': kind = TokenKind::Caret; break;
        case '~': kind = TokenKind::Tilde; break;
        case '?': kind = TokenKind::Question; break;
        case '.': kind = TokenKind::Period; break;
        default: kind = TokenKind::Invalid; break;
        }
        emit(kind, start);
    }

    const Source& src_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool in_pp_line_ = false;
    bool line_has_content_ = false;
    std::string ident_buffer_;
    LexOutput out_;
};

} // namespace

LexOutput lex(std::string_view text) {
    StringSource source{text};
    return Lexer<StringSource>(source).run();
}

LexOutput lex(const Text& text) {
    TextSource source(text);
    return Lexer<TextSource>(source).run();
}

} // namespace cind
