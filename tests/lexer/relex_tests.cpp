#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cpp_lexer/lexer.hpp"
#include "document/text.hpp"

#include <random>
#include <string>
#include <vector>

using namespace cind;

namespace {

// Applies one edit and checks relex against a from-scratch lex of the result.
std::string check_edit(const std::string& before, std::uint32_t start, std::uint32_t end,
                       std::string_view replacement) {
    Text old_text(before);
    LexOutput old_lex = lex(old_text);

    std::string after = before;
    after.replace(start, end - start, replacement);
    Text new_text(after);

    std::vector<TextEdit> edits;
    edits.push_back(TextEdit{make_range(start, end), std::string(replacement)});

    LexOutput incremental = relex(old_lex.tokens, old_lex.line_states, old_text, new_text, edits);
    LexOutput full = lex(new_text);

    REQUIRE(incremental.tokens.size() == full.tokens.size());
    for (std::size_t i = 0; i < full.tokens.size(); ++i) {
        CAPTURE(i);
        CHECK(incremental.tokens[i].kind == full.tokens[i].kind);
        CHECK(incremental.tokens[i].range == full.tokens[i].range);
        CHECK(incremental.tokens[i].flags == full.tokens[i].flags);
    }
    REQUIRE(incremental.line_states.size() == full.line_states.size());
    for (std::size_t i = 0; i < full.line_states.size(); ++i) {
        CAPTURE(i);
        CHECK(incremental.line_states[i] == full.line_states[i]);
    }
    return after;
}

} // namespace

TEST_CASE("relex: plain edits converge next line") {
    const std::string text = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\n";
    check_edit(text, 15, 16, "xyz");    // replace identifier on line 2
    check_edit(text, 11, 11, "  ");     // insert leading whitespace
    check_edit(text, 11, 22, "");       // delete a whole line
    check_edit(text, 22, 22, "f();\n"); // insert a whole line
    check_edit(text, 0, 0, "x");        // touch the very first byte
    check_edit(text, 43, 43, "int e;"); // append near EOF
}

TEST_CASE("relex: multi-line construct boundaries") {
    const std::string comment = "a();\n/* one\n   two\n   three */\nb();\n";
    check_edit(comment, 13, 13, "X"); // edit inside the block comment
    check_edit(comment, 26, 28, "");  // delete the "*/" -> comment runs on
    check_edit(comment, 5, 7, "");    // delete the "/*" -> lines become code

    const std::string raw = "auto s = R\"(line\nline)\";\nint x;\n";
    check_edit(raw, 14, 14, "Z"); // inside the raw string
    check_edit(raw, 21, 23, "");  // remove the closing delimiter
    check_edit(raw, 3, 3, "x");   // before the raw string, same line

    const std::string pp = "#define M(a) \\\n    (a + 1) \\\n    * 2\nint y;\n";
    check_edit(pp, 20, 20, "b");      // inside continuation line
    check_edit(pp, 13, 15, "");       // delete a splice -> pp line ends earlier
    check_edit(pp, 36, 36, " \\\nz"); // extend the continuation
}

TEST_CASE("relex: token joins at edit boundaries") {
    check_edit("a < b;\n c;\n", 3, 4, "");        // "< b" -> "<b" stays two tokens
    check_edit("a <\n= b;\n", 3, 4, "");          // deleting newline joins "<" "=" -> "<="
    check_edit("int R;\ns();\n", 5, 6, "\"x\"");  // R + string literal -> raw string
    check_edit("x = \"abc\";\ny();\n", 4, 5, ""); // unbalance a string quote
    check_edit("x = 1;\ny();\n", 5, 5, "'");      // stray quote, unterminated char
}

TEST_CASE("relex: whole-file and degenerate cases") {
    check_edit("", 0, 0, "int main() {}\n");
    check_edit("int main() {}\n", 0, 14, "");
    check_edit("x\n", 0, 2, "/* unterminated\ncomment");
    check_edit("one\n", 4, 4, "\n\n\n");
}

TEST_CASE("relex: fuzz against full lex") {
    // Deterministic; grows a document through random edits, checking each.
    std::mt19937 rng(20260714);
    const std::string_view fragments[] = {
        "int f(int a, int b) { return a + b; }\n",
        "/* block\n   comment */\n",
        "// line comment\n",
        "auto s = R\"delim(raw \"text\")delim\";\n",
        "#define MAX(a, b) ((a) > (b) ? (a) : (b)) \\\n    + 0\n",
        "\"string with \\\" escape\"",
        "template <typename T> class V { T* p; };\n",
        "x <<= y >>= z <=> w;\n",
        "'\\n'",
        "{",
        "}",
        "(",
        ")",
        "\"",
        "'",
        "\\\n",
        "\n",
        "R",
        "//",
        "/*",
        "*/",
        "#",
    };
    std::string doc = "int main() {\n    return 0;\n}\n";
    for (int step = 0; step < 400; ++step) {
        CAPTURE(step);
        const auto pick = fragments[rng() % std::size(fragments)];
        const auto len = static_cast<std::uint32_t>(doc.size());
        std::uint32_t start = len == 0 ? 0 : rng() % (len + 1);
        std::uint32_t end = start;
        if (rng() % 3 == 0 && start < len) {
            end = start + rng() % std::min<std::uint32_t>(len - start + 1, 40);
        }
        const bool insert = rng() % 4 != 0;
        doc = check_edit(doc, start, end, insert ? pick : std::string_view{});
        if (doc.size() > 8000) {
            doc.resize(2000); // keep the fuzz fast; truncation is itself an edit
            doc = check_edit(doc, static_cast<std::uint32_t>(doc.size()),
                             static_cast<std::uint32_t>(doc.size()), "\n");
        }
    }
}
