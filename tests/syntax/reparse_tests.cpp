#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cpp_lexer/lexer.hpp"
#include "document/text.hpp"
#include "syntax/green_node.hpp"
#include "syntax/syntax_tree.hpp"

#include <cstdlib>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace cind;

namespace {

// Materializing `green` against `t`'s tokens must reproduce `t` byte-exact:
// same DFS-preorder ids and per-node fields.
void expect_materializes_to(const GreenRef& green, const SyntaxTree& t, std::string_view text) {
    SyntaxTree back = flat_from_green(green, t.tokens());
    REQUIRE(back.node_count() == t.node_count());
    REQUIRE(back.tokens().size() == t.tokens().size());
    for (SyntaxNodeId id = 0; id < t.node_count(); ++id) {
        CAPTURE(id);
        const SyntaxNode& a = t.node(id);
        const SyntaxNode& b = back.node(id);
        REQUIRE(b.kind == a.kind);
        REQUIRE(b.first_token == a.first_token);
        REQUIRE(b.end_token == a.end_token);
        REQUIRE(b.parent == a.parent);
        REQUIRE(b.children == a.children);
        REQUIRE(b.incomplete == a.incomplete);
        REQUIRE(b.reclassified == a.reclassified);
        REQUIRE(b.expected == a.expected);
    }
    REQUIRE(back.dump(text) == t.dump(text));
}

// The green encoding round-trips (flat -> green -> flat), and the tree's own
// green_root() (maintained by parse/reparse) is consistent with its red nodes.
void check_green_roundtrip(const SyntaxTree& t, std::string_view text) {
    expect_materializes_to(green_from_flat(t), t, text);
    REQUIRE(t.green_root() != nullptr);
    expect_materializes_to(t.green_root(), t, text);
}

// Applies one edit via reparse() and checks against a from-scratch parse.
std::string check_edit(const std::string& before, std::uint32_t start, std::uint32_t end,
                       std::string_view replacement) {
    Text old_text(before);
    LexOutput lexed = lex(old_text);
    std::vector<LexerState> line_states = std::move(lexed.line_states);
    lexed.line_states.clear();
    SyntaxTree tree = parse(old_text, std::move(lexed));

    std::string after = before;
    after.replace(start, end - start, replacement);
    Text new_text(after);

    std::vector<TextEdit> edits;
    edits.push_back(TextEdit{make_range(start, end), std::string(replacement)});

    reparse(tree, line_states, old_text, new_text, edits);
    SyntaxTree full = parse(new_text);

    // Tokens must match a full lex (relex path), the tree a full parse.
    REQUIRE(tree.tokens().size() == full.tokens().size());
    for (std::size_t i = 0; i < full.tokens().size(); ++i) {
        CAPTURE(i);
        REQUIRE(tree.tokens()[i].kind == full.tokens()[i].kind);
        REQUIRE(tree.tokens()[i].range == full.tokens()[i].range);
        REQUIRE(tree.tokens()[i].flags == full.tokens()[i].flags);
    }
    const std::string got = tree.dump(after);
    const std::string want = full.dump(after);
    CHECK(got == want);
    REQUIRE(tree.node_count() == full.node_count());
    // dump() does not show parent links; verify them structurally.
    for (SyntaxNodeId id = 0; id < tree.node_count(); ++id) {
        CAPTURE(id);
        REQUIRE(tree.node(id).parent == full.node(id).parent);
        REQUIRE(tree.node(id).children == full.node(id).children);
        REQUIRE(tree.node(id).incomplete == full.node(id).incomplete);
        REQUIRE(tree.node(id).expected == full.node(id).expected);
    }
    check_green_roundtrip(full, after);
    check_green_roundtrip(tree, after);
    return after;
}

} // namespace

TEST_CASE("reparse: token-identical edits reuse the tree verbatim") {
    const std::string text = "int f() {\n    // note\n    return 1;\n}\nint g() { return 2; }\n";
    check_edit(text, 17, 17, " more");    // inside the comment
    check_edit(text, 14, 21, "");         // delete the comment body text
    const std::string str = "auto s = \"hello\";\nint x = 1;\n";
    check_edit(str, 11, 11, "big ");      // inside a string literal
}

TEST_CASE("reparse: statement edits stay inside one function") {
    const std::string text =
        "int f(int a) {\n    int x = a;\n    return x;\n}\n"
        "int g() {\n    return 42;\n}\n"
        "namespace ns {\nvoid h() {\n    h();\n}\n}\n";
    check_edit(text, 23, 24, "y");        // rename inside f
    check_edit(text, 31, 31, " + 1");     // extend an expression
    check_edit(text, 19, 30, "");         // delete a statement
    check_edit(text, 33, 33, "if (a) x++;\n    "); // insert a statement
    check_edit(text, 47, 47, "\n");       // blank line between functions
    check_edit(text, 90, 90, "x");        // inside namespace body
}

TEST_CASE("reparse: structural edits escalate correctly") {
    const std::string text =
        "int f() {\n    if (a) {\n        b();\n    }\n    c();\n}\n"
        "int tail() { return 0; }\n";
    check_edit(text, 41, 42, "");         // delete the if's closing '}'
    check_edit(text, 21, 22, "");         // delete the if's opening '{'
    check_edit(text, 8, 9, "");           // delete f's opening '{'
    check_edit(text, 53, 54, "");         // delete f's closing '}'
    check_edit(text, 30, 30, "} int split() { d();"); // split mid-function

    const std::string cls =
        "class C {\npublic:\n    void m();\nprivate:\n    int x_;\n};\n";
    check_edit(cls, 27, 27, " const");    // member edit
    check_edit(cls, 10, 17, "");          // delete "public:"
    check_edit(cls, 54, 55, "");          // delete the ';' after '}'

    const std::string sw =
        "void f(int v) {\n    switch (v) {\n    case 1:\n        a();\n        break;\n"
        "    case 2:\n        b();\n        break;\n    default:\n        c();\n    }\n}\n";
    check_edit(sw, 60, 60, "a();\n        "); // extra stmt in case 1
    check_edit(sw, 42, 43, "3");              // change the case label value
    check_edit(sw, 33, 41, "");               // delete "case 1:" entirely
}

TEST_CASE("reparse: enum bodies keep comma-separated items") {
    const std::string en = "enum class E {\n    A = 1,\n    B,\n    C\n};\nint after;\n";
    check_edit(en, 24, 24, "23");   // change an enumerator value
    check_edit(en, 31, 31, "2,\n    B");  // add an enumerator
    check_edit(en, 5, 5, "X");      // touch the head ("enum classX" -> opaque)
}

TEST_CASE("reparse: preprocessor and macro boundaries") {
    const std::string pp =
        "#define M(a) \\\n    ((a) + 1)\n"
        "int f() {\n    return M(2);\n}\n"
        "LLVM_MACRO(int, x)\n"
        "int g;\n";
    check_edit(pp, 22, 22, "* 2");   // inside the macro body
    check_edit(pp, 13, 15, "");      // remove the continuation splice
    check_edit(pp, 58, 58, "5, ");   // inside the macro invocation args
    check_edit(pp, 77, 77, ";");     // terminate the LLVM_MACRO declaration
}

TEST_CASE("reparse: template prefix lookahead guard") {
    // The '<' after `template` scans ahead for its '>'; an edit inside that
    // scan range must invalidate the prefix item too.
    const std::string t = "template <typename T\n>\nclass V {};\nint z;\n";
    check_edit(t, 21, 22, ";");      // '>' -> ';' : match_angles now fails
    const std::string u = "template <typename T;\nclass V {};\nint z;\n";
    check_edit(u, 20, 21, "\n>");    // ';' -> '>' : match now succeeds
}

TEST_CASE("reparse: edits at file boundaries") {
    const std::string text = "int a;\nint b;\n";
    check_edit(text, 0, 0, "// lead\n");
    check_edit(text, 14, 14, "int c;\n");
    check_edit(text, 0, 14, "void only() {}\n");
    check_edit("", 0, 0, "int x;\n");
    check_edit("int x;\n", 0, 7, "");
    // Unterminated constructs reaching EOF.
    check_edit(text, 7, 7, "/* open\n");
    check_edit("int a;\n/* open\nint b;\n", 7, 10, "");
}

TEST_CASE("reparse: fuzz against full parse") {
    // Multiple seeds: 20260715 exercises the CaseSection sibling-leading shift
    // (green splice); 50000 the zero-width-prefix-item boundary; the rest add
    // breadth. Each seed reruns the full edit loop below.
    const unsigned seeds[] = {20260715u, 50000u, 1013u, 424242u, 7u};
    for (unsigned seed : seeds) {
    CAPTURE(seed);
    std::mt19937 rng(seed);
    const std::string_view fragments[] = {
        "int f(int a, int b) { return a + b; }\n",
        "namespace ns {\nvoid g() {}\n}\n",
        "class C : public B {\npublic:\n    C() : x_(1) {}\n    int x_;\n};\n",
        "switch (v) {\ncase 1: a(); break;\ndefault: b();\n}\n",
        "enum E { A, B, C };\n",
        "template <typename T>\nstruct S { T t; };\n",
        "#define MAX(a, b) ((a) > (b) ? (a) : (b))\n",
        "if (x) {\n    y();\n} else {\n    z();\n}\n",
        "auto l = [](int q) { return q * 2; };\n",
        "extern \"C\" {\nvoid cfn(void);\n}\n",
        "for (int i = 0; i < n; ++i) { work(i); }\n",
        "do {\n    step();\n} while (cond);\n",
        "MACRO_CALL(a, b)\n",
        "int v = arr[idx];\n",
        "/* comment */", "// line\n", "{", "}", "(", ")", ";", ":", "<", ">", ",",
        "case 3:", "public:", "else", "\n", "    ", "x", "template", "enum",
    };
    std::string doc = "int main() {\n    return 0;\n}\n";
    for (int step = 0; step < 300; ++step) {
        CAPTURE(step);
        const auto pick = fragments[rng() % std::size(fragments)];
        const auto len = static_cast<std::uint32_t>(doc.size());
        std::uint32_t start = len == 0 ? 0 : rng() % (len + 1);
        std::uint32_t end = start;
        if (rng() % 3 == 0 && start < len) {
            end = start + rng() % std::min<std::uint32_t>(len - start + 1, 60);
        }
        const bool insert = rng() % 4 != 0;
        if (const char* dump_step = std::getenv("REPARSE_DUMP_STEP");
            dump_step != nullptr && std::stoi(dump_step) == step) {
            std::FILE* f = std::fopen("/tmp/reparse-repro.txt", "w");
            const std::string_view rep = insert ? pick : std::string_view{};
            std::fprintf(f, "%u %u %zu\n%.*s%s", start, end, rep.size(),
                         static_cast<int>(rep.size()), rep.data(), doc.c_str());
            std::fclose(f);
        }
        doc = check_edit(doc, start, end, insert ? pick : std::string_view{});
        if (doc.size() > 12000) {
            doc = check_edit(doc, 0, static_cast<std::uint32_t>(doc.size()) / 2, "");
        }
    }
    } // seeds
}

TEST_CASE("reparse: edits around split-brace #if/#else match a full parse") {
    // Both branches open a brace, one shared '}' after #endif. A reparse whose
    // span starts between #if and #else must not flatten the sibling structure
    // the full parse produces (design.md §276).
    const std::string text = "int h() {\n"    // 0
                             "#ifdef A\n"      // 1
                             "  int x = 1;\n"  // 2
                             "  if (a) {\n"    // 3
                             "#else\n"         // 4
                             "  int y = 2;\n"  // 5
                             "  if (b) {\n"    // 6
                             "#endif\n"        // 7
                             "    inner();\n"  // 8
                             "  }\n"           // 9
                             "  tail();\n"     // 10
                             "}\n";            // 11
    // edit inside the #if branch (before #else): the risky mid-conditional start
    check_edit(text, 30, 31, "11");  // int x = 1 -> int x = 11
    // edit inside the #else branch
    check_edit(text, 56, 57, "22");
    // edit in the shared tail after #endif
    check_edit(text, 92, 96, "call");
    // edit on the last statement
    check_edit(text, 108, 112, "ret");
}

TEST_CASE("reparse: edits near the extern-C / __cplusplus guard match a full parse") {
    const std::string text = "#ifdef __cplusplus\n"
                             "extern \"C\" {\n"
                             "#endif\n"
                             "void f(int);\n"
                             "int g(void);\n"
                             "#ifdef __cplusplus\n"
                             "}\n"
                             "#endif\n";
    check_edit(text, 50, 51, "F");   // rename inside the guarded body
    check_edit(text, 63, 63, "\nvoid h(void);"); // insert a declaration
}

TEST_CASE("reparse fuzz: split-brace conditionals stay consistent under random edits") {
    std::mt19937 rng(0xC0FFEE);
    const char* fragments[] = {
        "#ifdef A\n", "#else\n", "#endif\n", "#if X\n", "#elif Y\n",
        "if (a) {\n", "if (b) {\n", "}\n", "  stmt();\n", "  return x;\n",
        "int z = 1;\n", "void q() {\n", "for (;;) {\n", ";", "{", "}",
        "// c\n", "x", "\n", "  ",
    };
    std::string doc = "int h() {\n"
                      "#ifdef A\n  if (a) {\n#else\n  if (b) {\n#endif\n"
                      "    inner();\n  }\n  tail();\n}\n";
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
            doc = check_edit(doc, 0, static_cast<std::uint32_t>(doc.size()) / 2, "");
        }
    }
}

TEST_CASE("reparse: minimized pp-fuzz repro (step 153)") {
    const std::string doc = "; } ;;##endif\ni#else\n{#e\n}\nx;void#i X\nq() {\n#elif Y\n  // c\n";
    check_edit(doc, 27, 27, "#ifdef A\n");
}

TEST_CASE("reparse: minimized pp-fuzz repro (seed5 step268)") {
    const std::string doc =
        "#  ifdef Ax\nt#int v#else\noid q()#endif\n {\nz =    for (;;) {\n(ifurn x;\n#if X\nt//"
        "}oivoid q() if (a) {\n{\nf (a) {\n#else\nid q() {\nturn x;\nnt #i;xe#ifdef A\n;\n(  "
        "stmtvoid q() {\n\nif (#else\nif (b) {\nb) {\n)inxt z  //void q() {\n\n{\n {\n();\ne#endif\n";
    check_edit(doc, 214, 223, "  stmt();\n");
}

TEST_CASE("reparse: minimized pp-fuzz repro (seed11 step463)") {
    const std::string doc =
        "#if X\nif    int x#ifdef A\nc\nivoid q\n#if X\ntm};t(})void q(#else\n) {for#{else\n (;;) "
        "{if (a) {\nurn x;\nor (;;) {\n#elif Y\n\nq(}}\n) {#endif\n\n {\n  #  return x;\n (a) {\n"
        "f int z = 1;\nlif Y\n{\n X\n";
    check_edit(doc, 16, 16, "for (;;) {\n");
}
