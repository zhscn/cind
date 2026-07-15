#include "cpp_lexer/lexer.hpp"

#include "document/char_source.hpp"
#include "document/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
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

// Alignment oracle for incremental relex: past `min_pos`, the scanner asks at
// every clean line start (fresh default state, between tokens) whether the
// old lex agrees at the corresponding old offset. Answering true stops the
// scan; the caller splices the old suffix.
struct StopCheck {
    std::size_t min_pos = 0;
    // new-text position -> converged? Only called at clean line starts.
    virtual bool converged(std::size_t new_pos) = 0;
    virtual ~StopCheck() = default;
};

template <typename Source>
class Lexer {
public:
    explicit Lexer(const Source& source) : src_(source), size_(source.size()) {}

    // Restart mode: begin scanning at `start`, which must be a line start
    // whose entry state is default. No initial line state is recorded (the
    // caller stitches line_states); `stop` may end the scan early.
    Lexer(const Source& source, std::size_t start, StopCheck* stop)
        : src_(source), size_(source.size()), pos_(start), at_line_start_(true), stop_(stop) {}

    LexOutput run() {
        if (stop_ == nullptr) {
            out_.line_states.emplace_back();
        }
        while (pos_ < size_) {
            if (at_line_start_ && !in_pp_line_ && stop_ != nullptr && pos_ >= stop_->min_pos &&
                stop_->converged(pos_)) {
                stopped_at_ = pos_;
                return std::move(out_);
            }
            lex_one();
        }
        emit(TokenKind::EndOfFile, pos_);
        return std::move(out_);
    }

    // Position where the stop check fired; size_ (or beyond) means the scan
    // ran to end of input.
    std::size_t stopped_at() const { return stopped_at_; }

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
        at_line_start_ = false;
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
            at_line_start_ = true;
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
            at_line_start_ = true; // physically at a line start (mid-splice)
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
    bool at_line_start_ = false;
    StopCheck* stop_ = nullptr;
    std::size_t stopped_at_ = static_cast<std::size_t>(-1);
    std::string ident_buffer_;
    LexOutput out_;
};

} // namespace

LexOutput lex(std::string_view text) {
    StringCharSource source{text};
    return Lexer<StringCharSource>(source).run();
}

LexOutput lex(const Text& text) {
    TextCharSource source(text);
    return Lexer<TextCharSource>(source).run();
}

namespace {

constexpr std::size_t kNoToken = static_cast<std::size_t>(-1);

// Replaces v[first, last) with `replacement`, moving the tail once.
template <typename T>
void splice_vector(std::vector<T>& v, std::size_t first, std::size_t last,
                   std::vector<T>& replacement) {
    const std::size_t old_len = last - first;
    const std::size_t new_len = replacement.size();
    if (new_len <= old_len) {
        std::move(replacement.begin(), replacement.end(),
                  v.begin() + static_cast<std::ptrdiff_t>(first));
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(first + new_len),
                v.begin() + static_cast<std::ptrdiff_t>(last));
    } else {
        std::move(replacement.begin(), replacement.begin() + static_cast<std::ptrdiff_t>(old_len),
                  v.begin() + static_cast<std::ptrdiff_t>(first));
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(last),
                 std::make_move_iterator(replacement.begin() + static_cast<std::ptrdiff_t>(old_len)),
                 std::make_move_iterator(replacement.end()));
    }
}

// Index of the token starting exactly at `off`, or kNoToken. Tokens are
// contiguous and sorted; only EndOfFile is zero-length.
std::size_t token_index_at(const TokenBuffer& tokens, std::size_t off) {
    std::size_t lo = 0;
    std::size_t hi = tokens.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (tokens[mid].range.start.value < off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < tokens.size() && tokens[lo].range.start.value == off) {
        return lo;
    }
    return kNoToken;
}

// Convergence oracle: true at a new-text clean line start whose corresponding
// old offset is also a clean line start (default entry state) beginning a
// token. From there on the old lex is valid verbatim, shifted by delta.
struct AlignCheck final : StopCheck {
    const TokenBuffer* old_tokens = nullptr;
    const std::vector<LexerState>* old_line_states = nullptr;
    const Text* old_text = nullptr;
    std::int64_t delta = 0;

    std::size_t stop_token = kNoToken; // outputs, valid once converged
    std::uint32_t stop_line = 0;

    bool converged(std::size_t new_pos) override {
        const std::int64_t old_pos = static_cast<std::int64_t>(new_pos) - delta;
        if (old_pos < 0 || old_pos >= static_cast<std::int64_t>(old_text->size_bytes())) {
            return false;
        }
        const TextOffset off{static_cast<std::uint32_t>(old_pos)};
        const LinePosition pos = old_text->position(off);
        if (pos.byte_column != 0 || !((*old_line_states)[pos.line] == LexerState{})) {
            return false;
        }
        const std::size_t tok = token_index_at(*old_tokens, off.value);
        if (tok == kNoToken) {
            return false;
        }
        stop_token = tok;
        stop_line = pos.line;
        return true;
    }
};

} // namespace

RelexSplice relex_scan(const TokenBuffer& old_tokens,
                       const std::vector<LexerState>& old_line_states, const Text& old_text,
                       const Text& new_text, std::span<const TextEdit> edits) {
    // Merged damage window: one hunk from the first edit to the last. The
    // offset shift is uniform past the last edit, which is the only region
    // where old coordinates are consulted again.
    const std::size_t damage_old_start = edits.front().old_range.start.value;
    const std::size_t damage_old_end = edits.back().old_range.end.value;
    std::int64_t delta = 0;
    for (const TextEdit& e : edits) {
        delta += static_cast<std::int64_t>(e.new_text.size()) -
                 static_cast<std::int64_t>(e.old_range.length());
    }
    const std::size_t damage_new_end =
        static_cast<std::size_t>(static_cast<std::int64_t>(damage_old_end) + delta);

    // Restart point: walk back to a line entered with default state that
    // begins at a token boundary. The former rules out block comments, raw
    // strings and pp continuations spanning into the line; the latter rules
    // out a backslash-newline splice token spanning the line start.
    std::uint32_t restart_line = old_text.position(TextOffset{static_cast<std::uint32_t>(
        std::min<std::size_t>(damage_old_start, old_text.size_bytes()))}).line;
    while (restart_line > 0 &&
           !(old_line_states[restart_line] == LexerState{} &&
             token_index_at(old_tokens, old_text.line_start(restart_line).value) != kNoToken)) {
        --restart_line;
    }
    const std::size_t restart_off = old_text.line_start(restart_line).value;

    AlignCheck align;
    align.min_pos = damage_new_end;
    align.old_tokens = &old_tokens;
    align.old_line_states = &old_line_states;
    align.old_text = &old_text;
    align.delta = delta;

    TextCharSource source(new_text);
    Lexer<TextCharSource> scanner(source, restart_off, &align);

    RelexSplice s;
    s.scanned = scanner.run();
    s.keep_tokens = token_index_at(old_tokens, restart_off);
    s.restart_line = restart_line;
    s.delta = delta;
    s.hit_eof = align.stop_token == kNoToken;
    s.stop_token = s.hit_eof ? old_tokens.size() : align.stop_token;
    s.stop_line =
        s.hit_eof ? static_cast<std::uint32_t>(old_line_states.size() - 1) : align.stop_line;
    return s;
}

LexOutput relex(const std::vector<Token>& old_tokens,
                const std::vector<LexerState>& old_line_states, const Text& old_text,
                const Text& new_text, std::span<const TextEdit> edits) {
    if (edits.empty()) {
        return LexOutput{old_tokens, old_line_states};
    }
    RelexSplice s =
        relex_scan(TokenBuffer(old_tokens), old_line_states, old_text, new_text, edits);

    LexOutput result;
    result.tokens.reserve(s.keep_tokens + s.scanned.tokens.size() +
                          (old_tokens.size() - s.stop_token));
    result.tokens.insert(result.tokens.end(), old_tokens.begin(),
                         old_tokens.begin() + static_cast<std::ptrdiff_t>(s.keep_tokens));
    result.tokens.insert(result.tokens.end(), s.scanned.tokens.begin(), s.scanned.tokens.end());
    for (std::size_t i = s.stop_token; i < old_tokens.size(); ++i) {
        Token t = old_tokens[i];
        t.range.start.value = static_cast<std::uint32_t>(t.range.start.value + s.delta);
        t.range.end.value = static_cast<std::uint32_t>(t.range.end.value + s.delta);
        result.tokens.push_back(t);
    }

    result.line_states.reserve(new_text.line_count());
    result.line_states.insert(result.line_states.end(), old_line_states.begin(),
                              old_line_states.begin() + s.restart_line + 1);
    result.line_states.insert(result.line_states.end(),
                              std::make_move_iterator(s.scanned.line_states.begin()),
                              std::make_move_iterator(s.scanned.line_states.end()));
    // The scanner already pushed the entry state of the stop line when it
    // consumed the newline before it; the old suffix starts one later.
    if (!s.hit_eof) {
        result.line_states.insert(result.line_states.end(),
                                  old_line_states.begin() + s.stop_line + 1,
                                  old_line_states.end());
    }
    return result;
}

void relex_apply(TokenBuffer& tokens, std::vector<LexerState>& line_states, RelexSplice&& s) {
    // One structural pass: replace the window and rebase the suffix chunks by
    // delta — O(chunk count + window), the flat vector's O(token count)
    // suffix shift and memmove are gone (design.md §214).
    tokens.splice(s.keep_tokens, s.stop_token, s.scanned.tokens, s.delta);
    splice_vector(line_states, s.restart_line + 1,
                  s.hit_eof ? line_states.size() : s.stop_line + 1, s.scanned.line_states);
}

RelexSplice relex_in_place(TokenBuffer& tokens, std::vector<LexerState>& line_states,
                           const Text& old_text, const Text& new_text,
                           std::span<const TextEdit> edits) {
    if (edits.empty()) {
        return RelexSplice{tokens.size(), tokens.size(),
                           static_cast<std::uint32_t>(line_states.size() - 1),
                           static_cast<std::uint32_t>(line_states.size() - 1),
                           0,
                           false,
                           {}};
    }
    RelexSplice s = relex_scan(tokens, line_states, old_text, new_text, edits);
    RelexSplice out{s.keep_tokens, s.stop_token, s.restart_line, s.stop_line,
                    s.delta,       s.hit_eof,    {}}; // scanned stays with the apply
    relex_apply(tokens, line_states, std::move(s));
    return out;
}

} // namespace cind
