#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cpp_lexer/lexer.hpp"

#include <random>
#include <string>
#include <vector>

using namespace cind;

namespace {

// design.md §14.2: contiguous coverage, no overlap, concat == input,
// EndOfFile terminator.
void check_invariants(std::string_view text, const LexOutput& out) {
    REQUIRE(!out.tokens.empty());
    std::uint32_t cursor = 0;
    for (const Token& token : out.tokens) {
        REQUIRE(token.range.start.value == cursor);
        REQUIRE(token.range.end.value >= token.range.start.value);
        cursor = token.range.end.value;
    }
    REQUIRE(cursor == text.size());
    const Token& eof = out.tokens.back();
    CHECK(eof.kind == TokenKind::EndOfFile);
    CHECK(eof.range.length() == 0);

    std::size_t newline_count = 0;
    for (char c : text) {
        if (c == '\n') {
            ++newline_count;
        }
    }
    CHECK(out.line_states.size() == newline_count + 1);
}

std::vector<TokenKind> kinds_of(std::string_view text, bool keep_trivia = false) {
    LexOutput out = lex(text);
    check_invariants(text, out);
    std::vector<TokenKind> kinds;
    for (const Token& token : out.tokens) {
        if (token.kind == TokenKind::EndOfFile) {
            continue;
        }
        if (!keep_trivia && is_trivia(token.kind)) {
            continue;
        }
        kinds.push_back(token.kind);
    }
    return kinds;
}

const Token& token_at(const LexOutput& out, std::size_t non_trivia_index) {
    std::size_t seen = 0;
    for (const Token& token : out.tokens) {
        if (is_trivia(token.kind) || token.kind == TokenKind::EndOfFile) {
            continue;
        }
        if (seen == non_trivia_index) {
            return token;
        }
        ++seen;
    }
    FAIL("token index out of range");
    return out.tokens.back();
}

} // namespace

TEST_CASE("empty input") {
    LexOutput out = lex("");
    check_invariants("", out);
    CHECK(out.tokens.size() == 1);
}

TEST_CASE("basic declaration") {
    using enum TokenKind;
    CHECK(kinds_of("namespace foo { int x; }") ==
          std::vector<TokenKind>{NamespaceKw, Identifier, LBrace, Identifier, Identifier,
                                 Semicolon, RBrace});
}

TEST_CASE("keywords are exact matches") {
    using enum TokenKind;
    CHECK(kinds_of("classic class classes") ==
          std::vector<TokenKind>{Identifier, ClassKw, Identifier});
    CHECK(kinds_of("switch case default public private protected") ==
          std::vector<TokenKind>{SwitchKw, CaseKw, DefaultKw, PublicKw, PrivateKw, ProtectedKw});
}

TEST_CASE("maximal munch punctuators") {
    using enum TokenKind;
    CHECK(kinds_of("a<<b") == std::vector<TokenKind>{Identifier, LessLess, Identifier});
    CHECK(kinds_of("a<b>c") ==
          std::vector<TokenKind>{Identifier, Less, Identifier, Greater, Identifier});
    CHECK(kinds_of("x::y") == std::vector<TokenKind>{Identifier, ColonColon, Identifier});
    CHECK(kinds_of("x: y") == std::vector<TokenKind>{Identifier, Colon, Identifier});
    CHECK(kinds_of("p->q") == std::vector<TokenKind>{Identifier, Arrow, Identifier});
    CHECK(kinds_of("p->*q") == std::vector<TokenKind>{Identifier, ArrowStar, Identifier});
    CHECK(kinds_of("a<=>b") == std::vector<TokenKind>{Identifier, Spaceship, Identifier});
    CHECK(kinds_of("a=b==c") ==
          std::vector<TokenKind>{Identifier, Equals, Identifier, EqualEqual, Identifier});
    CHECK(kinds_of("f(...)") == std::vector<TokenKind>{Identifier, LParen, Ellipsis, RParen});
    CHECK(kinds_of(">>=") == std::vector<TokenKind>{GreaterGreaterEqual});
    CHECK(kinds_of("a##b") == std::vector<TokenKind>{Identifier, Punctuator, Identifier});
}

TEST_CASE("string and character literals") {
    using enum TokenKind;
    CHECK(kinds_of(R"("hi \"there\"" 'x' '\'')") ==
          std::vector<TokenKind>{StringLiteral, CharacterLiteral, CharacterLiteral});
    CHECK(kinds_of("u8\"x\" L\"y\" U'c'") ==
          std::vector<TokenKind>{StringLiteral, StringLiteral, CharacterLiteral});

    SUBCASE("unterminated string stops at the newline") {
        LexOutput out = lex("\"open\nnext");
        check_invariants("\"open\nnext", out);
        CHECK(out.tokens[0].kind == StringLiteral);
        CHECK(has_flag(out.tokens[0].flags, LexicalFlags::Unterminated));
        CHECK(out.tokens[0].range == make_range(0, 5)); // '\n' stays outside
        CHECK(out.tokens[1].kind == Newline);
    }
}

TEST_CASE("raw string literals") {
    using enum TokenKind;
    SUBCASE("multi-line with delimiter") {
        std::string text = "auto s = R\"ab(line1\n)x\nline2)ab\";";
        LexOutput out = lex(text);
        check_invariants(text, out);
        const Token& raw = token_at(out, 3);
        CHECK(raw.kind == RawStringLiteral);
        CHECK(!has_flag(raw.flags, LexicalFlags::Unterminated));
        // line_states inside the raw string carry the delimiter
        REQUIRE(out.line_states.size() == 3);
        CHECK(out.line_states[1].inside_raw_string);
        CHECK(out.line_states[1].raw_delimiter == "ab");
        CHECK(out.line_states[2].inside_raw_string);
    }
    SUBCASE("prefixed") {
        CHECK(kinds_of("u8R\"(x)\"") == std::vector<TokenKind>{RawStringLiteral});
        // FooR is an identifier, not a raw-string prefix
        CHECK(kinds_of("FooR\"(x)\"") ==
              std::vector<TokenKind>{Identifier, StringLiteral});
    }
    SUBCASE("unterminated runs to EOF") {
        std::string text = "R\"d(open\nstill";
        LexOutput out = lex(text);
        check_invariants(text, out);
        CHECK(out.tokens[0].kind == RawStringLiteral);
        CHECK(has_flag(out.tokens[0].flags, LexicalFlags::Unterminated));
        CHECK(out.line_states[1].inside_raw_string);
    }
    SUBCASE("malformed delimiter degrades to a string literal") {
        std::string text = "R\"a b(x)\"";
        LexOutput out = lex(text);
        check_invariants(text, out);
        CHECK(out.tokens[0].kind == StringLiteral);
    }
}

TEST_CASE("comments") {
    using enum TokenKind;
    SUBCASE("line comment without trailing newline") {
        LexOutput out = lex("int x; // tail");
        check_invariants("int x; // tail", out);
        CHECK(out.tokens[out.tokens.size() - 2].kind == LineComment);
    }
    SUBCASE("multi-line block comment states") {
        std::string text = "a /* one\ntwo\n*/ b";
        LexOutput out = lex(text);
        check_invariants(text, out);
        REQUIRE(out.line_states.size() == 3);
        CHECK(out.line_states[1].inside_block_comment);
        CHECK(out.line_states[2].inside_block_comment);
    }
    SUBCASE("unterminated block comment") {
        LexOutput out = lex("/* open\n");
        check_invariants("/* open\n", out);
        CHECK(out.tokens[0].kind == BlockComment);
        CHECK(has_flag(out.tokens[0].flags, LexicalFlags::Unterminated));
    }
}

TEST_CASE("numbers use the pp-number rule") {
    using enum TokenKind;
    CHECK(kinds_of("1'000'000 0x1.8p-3 1.5e+10f 0b1010 .5") ==
          std::vector<TokenKind>{Number, Number, Number, Number, Number});
    CHECK(kinds_of("1+2") == std::vector<TokenKind>{Number, Plus, Number});
    CHECK(kinds_of("x-1") == std::vector<TokenKind>{Identifier, Minus, Number});
}

TEST_CASE("preprocessor lines") {
    SUBCASE("directive tokens are flagged") {
        std::string text = "#include <foo>\nint x;";
        LexOutput out = lex(text);
        check_invariants(text, out);
        CHECK(out.tokens[0].kind == TokenKind::PreprocessorHash);
        CHECK(has_flag(out.tokens[0].flags, LexicalFlags::PreprocessorLine));
        const Token& include_word = token_at(out, 1);
        CHECK(has_flag(include_word.flags, LexicalFlags::PreprocessorLine));
        // after the newline the flag is gone
        const Token& int_kw = token_at(out, 5);
        CHECK(!has_flag(int_kw.flags, LexicalFlags::PreprocessorLine));
    }
    SUBCASE("hash after leading whitespace still opens a directive") {
        std::string text = "   #define X 1";
        LexOutput out = lex(text);
        check_invariants(text, out);
        CHECK(token_at(out, 0).kind == TokenKind::PreprocessorHash);
    }
    SUBCASE("hash mid-line is a punctuator") {
        std::string text = "int a; #x";
        LexOutput out = lex(text);
        check_invariants(text, out);
        CHECK(token_at(out, 3).kind == TokenKind::Punctuator);
    }
    SUBCASE("splice continues the directive") {
        std::string text = "#define M(x) \\\n    x\nint y;";
        LexOutput out = lex(text);
        check_invariants(text, out);
        REQUIRE(out.line_states.size() == 3);
        CHECK(out.line_states[1].preprocessor_continuation);
        CHECK(!out.line_states[2].preprocessor_continuation);
        // the continuation-line body token is still flagged
        const Token& body_x = token_at(out, 6);
        CHECK(has_flag(body_x.flags, LexicalFlags::PreprocessorLine));
        const Token& int_kw = token_at(out, 7);
        CHECK(!has_flag(int_kw.flags, LexicalFlags::PreprocessorLine));
    }
    SUBCASE("stringize and paste inside a directive") {
        std::string text = "#define S(x) #x\n#define P(a,b) a##b";
        LexOutput out = lex(text);
        check_invariants(text, out);
    }
}

TEST_CASE("invalid bytes become Invalid tokens") {
    using enum TokenKind;
    CHECK(kinds_of("a @ b") == std::vector<TokenKind>{Identifier, Invalid, Identifier});
    CHECK(kinds_of("`") == std::vector<TokenKind>{Invalid});
    // lone backslash (not a splice)
    CHECK(kinds_of("a \\ b") == std::vector<TokenKind>{Identifier, Invalid, Identifier});
    // NUL byte
    std::string with_nul = "a";
    with_nul.push_back('\0');
    with_nul += "b";
    LexOutput out = lex(with_nul);
    check_invariants(with_nul, out);
    CHECK(out.tokens[1].kind == Invalid);
}

TEST_CASE("operator keyword and template brackets") {
    using enum TokenKind;
    CHECK(kinds_of("operator<<") == std::vector<TokenKind>{OperatorKw, LessLess});
    CHECK(kinds_of("vector<vector<int>> v") ==
          std::vector<TokenKind>{Identifier, Less, Identifier, Less, Identifier, GreaterGreater,
                                 Identifier});
}

TEST_CASE("fuzz: arbitrary bytes never break the invariants") {
    std::mt19937 rng(987654321);
    std::uniform_int_distribution<int> len_dist(0, 200);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> mode_dist(0, 2);
    // A C++-flavored alphabet reaches deeper lexer paths than raw bytes.
    static constexpr std::string_view kCppish =
        " \t\nabcR\"'(){}[]<>:;,#\\/*=+-.0123456789_";

    for (int round = 0; round < 400; ++round) {
        const int mode = mode_dist(rng);
        std::string text;
        const int len = len_dist(rng);
        for (int i = 0; i < len; ++i) {
            if (mode == 0) {
                text.push_back(static_cast<char>(byte_dist(rng)));
            } else {
                text.push_back(
                    kCppish[static_cast<std::size_t>(byte_dist(rng)) % kCppish.size()]);
            }
        }
        if (text.find('\r') != std::string::npos) {
            continue; // documents never contain '\r' (normalized on load)
        }
        LexOutput out = lex(text);
        check_invariants(text, out);
    }
}
