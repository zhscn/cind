#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "commands/editor_commands.hpp"

#include <string>
#include <utility>

using namespace cind;

namespace {

// Fixture format from design.md §13.3: '^' marks the caret.
std::pair<std::string, TextOffset> parse_caret(std::string_view fixture) {
    std::size_t pos = fixture.find('^');
    REQUIRE(pos != std::string_view::npos);
    std::string text(fixture);
    text.erase(pos, 1);
    return {std::move(text), TextOffset{static_cast<std::uint32_t>(pos)}};
}

// Applies Enter at '^', returns the new text with '^' at the final caret.
std::string enter(std::string_view fixture, const CppIndentStyle& style = {}) {
    auto [text, caret] = parse_caret(fixture);
    Document doc(std::move(text));
    EnterResult result = press_enter(doc, caret, style);
    std::string out = doc.snapshot().content().to_string();
    out.insert(result.caret.value, "^");
    return out;
}

// Type `input` at the caret (plain insert), returning the new caret.
TextOffset type_text(Document& doc, TextOffset caret, std::string_view input) {
    auto tx = doc.begin_transaction();
    tx.insert(caret, input);
    tx.commit();
    return TextOffset{caret.value + static_cast<std::uint32_t>(input.size())};
}

// Feeds `input` through the typed-char pipeline; returns text with '^'.
std::string type_chars(std::string_view fixture, std::string_view input,
                       const CppIndentStyle& style = {}) {
    auto [text, caret] = parse_caret(fixture);
    Document doc(std::move(text));
    TextOffset c = caret;
    for (char ch : input) {
        c = type_char(doc, c, ch, style).caret;
    }
    std::string out = doc.snapshot().content().to_string();
    out.insert(c.value, "^");
    return out;
}

} // namespace

TEST_CASE("namespace body is not indented by default") {
    CHECK(enter("namespace foo {^\nint x;\n}\n") == "namespace foo {\n^\nint x;\n}\n");

    CppIndentStyle indented;
    indented.namespace_indentation = CppIndentStyle::NamespaceIndentation::All;
    CHECK(enter("namespace foo {^\nint x;\n}\n", indented) ==
          "namespace foo {\n    ^\nint x;\n}\n");
}

TEST_CASE("Inner namespace indentation indents only nested namespace bodies") {
    CppIndentStyle inner;
    inner.namespace_indentation = CppIndentStyle::NamespaceIndentation::Inner;
    // Outer body stays flush (clang-format NamespaceIndentation: Inner).
    CHECK(enter("namespace out {^\n}\n", inner) == "namespace out {\n^\n}\n");
    // The nested namespace's body indents relative to its own opening line.
    CHECK(enter("namespace out {\nnamespace in {^\n}\n}\n", inner) ==
          "namespace out {\nnamespace in {\n    ^\n}\n}\n");
}

TEST_CASE("enter between braces expands and places the caret on the middle line") {
    CHECK(enter("void f() {^}\n") == "void f() {\n    ^\n}\n");
    CHECK(enter("namespace foo {^}\n") == "namespace foo {\n^\n}\n");
    CHECK(enter("class Foo {^};\n") == "class Foo {\n    ^\n};\n");
    // nested: closing brace aligns with the opening line's indent
    CHECK(enter("void f() {\n    if (x) {^}\n}\n") ==
          "void f() {\n    if (x) {\n        ^\n    }\n}\n");
    // lambda argument
    CHECK(enter("foo([&](int x) {^});\n") == "foo([&](int x) {\n    ^\n});\n");
}

TEST_CASE("enter between braces works regardless of how the caret got there") {
    // The braces were typed long ago; predicate is structural, not history.
    std::string text = "void f() {}\n";
    Document doc(text);
    EnterResult result = press_enter(doc, TextOffset{10}, CppIndentStyle{});
    CHECK(result.handler == "EnterBetweenBraces");
    CHECK(doc.snapshot().content() == "void f() {\n    \n}\n");
    // one undo unit reverts the whole expansion
    REQUIRE(doc.undo().has_value());
    CHECK(doc.snapshot().content() == text);
}

TEST_CASE("class body and access specifiers") {
    CHECK(enter("class Foo {\npublic:^\n};\n") == "class Foo {\npublic:\n    ^\n};\n");

    // reindenting the access specifier line keeps it at the class column
    Document doc("class Foo {\n        public:\n};\n");
    IndentDecision d = indent_line(doc, 1, CppIndentStyle{});
    CHECK(d.role == FormatRole::AccessSpecifierLabel);
    CHECK(doc.snapshot().content() == "class Foo {\npublic:\n};\n");

    CppIndentStyle offset_style;
    offset_style.access_specifier_offset = 2;
    Document doc2("class Foo {\npublic:\n};\n");
    indent_line(doc2, 1, offset_style);
    CHECK(doc2.snapshot().content() == "class Foo {\n  public:\n};\n");
}

TEST_CASE("switch, case labels and case bodies") {
    // statement after a label gets case-body indent
    CHECK(enter("switch (x) {\ncase 1:^\n}\n") == "switch (x) {\ncase 1:\n    ^\n}\n");
    // after a statement, stay at case-body indent
    CHECK(enter("switch (x) {\ncase 1:\n    foo();^\n}\n") ==
          "switch (x) {\ncase 1:\n    foo();\n    ^\n}\n");

    // a mis-indented new label dedents back to the switch column
    Document doc("switch (x) {\ncase 1:\n    foo();\n    case 2:\n}\n");
    IndentDecision d = indent_line(doc, 3, CppIndentStyle{});
    CHECK(d.role == FormatRole::CaseLabel);
    CHECK(doc.snapshot().content() == "switch (x) {\ncase 1:\n    foo();\ncase 2:\n}\n");

    CppIndentStyle both;
    both.indent_case_label = true;
    Document doc2("switch (x) {\ncase 1:\n}\n");
    indent_line(doc2, 1, both);
    CHECK(doc2.snapshot().content() == "switch (x) {\n    case 1:\n}\n");
}

TEST_CASE("constructor initializer list, typed step by step") {
    // design.md §13.3 sequence fixture
    Document doc("Foo::Foo()");
    TextOffset caret{10};

    EnterResult first = press_enter(doc, caret, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "Foo::Foo()\n    ");
    caret = type_text(doc, first.caret, ": a_(1),");

    EnterResult second = press_enter(doc, caret, CppIndentStyle{});
    CHECK(second.decision.role == FormatRole::ConstructorInitializerItem);
    type_text(doc, second.caret, "b_(2)");

    CHECK(doc.snapshot().content() == "Foo::Foo()\n    : a_(1),\n      b_(2)");
}

TEST_CASE("bare colon initializer keeps a stable continuation indent") {
    CHECK(enter("Foo::Foo()\n    :^\n") == "Foo::Foo()\n    :\n    ^\n");
}

TEST_CASE("comma-prepended constructor initializers align with the colon") {
    // clang-format BreakConstructorInitializers: BeforeComma (WebKit/Mozilla)
    CppIndentStyle style;
    style.constructor_initializers =
        CppIndentStyle::ConstructorInitializerStyle::AlignWithColon;

    Document doc("Foo::Foo()");
    TextOffset caret{10};
    EnterResult first = press_enter(doc, caret, style);
    caret = type_text(doc, first.caret, ": a_(1)");

    EnterResult second = press_enter(doc, caret, style);
    CHECK(second.decision.role == FormatRole::ConstructorInitializerItem);
    type_text(doc, second.caret, ", b_(2) {}");

    CHECK(doc.snapshot().content() == "Foo::Foo()\n    : a_(1)\n    , b_(2) {}");
}

TEST_CASE("constructor initializer styles") {
    std::string text = "Foo::Foo()\n    : a_(1),^\n{\n}\n";

    CppIndentStyle normal;
    normal.constructor_initializers = CppIndentStyle::ConstructorInitializerStyle::NormalIndent;
    CHECK(enter(text, normal) == "Foo::Foo()\n    : a_(1),\n    ^\n{\n}\n");

    CppIndentStyle after_colon;
    after_colon.constructor_initializers =
        CppIndentStyle::ConstructorInitializerStyle::AlignAfterColon;
    CHECK(enter(text, after_colon) == "Foo::Foo()\n    : a_(1),\n      ^\n{\n}\n");
}

TEST_CASE("declaration-prefix macro does not disturb initializer alignment") {
    CHECK(enter("MY_API\nFoo::Foo()\n    : a_(1),^\n{\n}\n") ==
          "MY_API\nFoo::Foo()\n    : a_(1),\n      ^\n{\n}\n");
}

TEST_CASE("if without braces indents one level and closes back") {
    CHECK(enter("if (x)^\n") == "if (x)\n    ^\n");
    CHECK(enter("if (x)\n    foo();^\n") == "if (x)\n    foo();\n^\n");
    CHECK(enter("if (a)\n    f();\nelse^\n") == "if (a)\n    f();\nelse\n    ^\n");

    // 'else' itself aligns with its if
    Document doc("if (a)\n    f();\n        else\n    g();\n");
    IndentDecision d = indent_line(doc, 2, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "if (a)\n    f();\nelse\n    g();\n");
    CHECK(d.target_column == 0);
}

TEST_CASE("T3: adjacent string literals align to the run start") {
    Document doc("  return \"aaa\"\n\"bbb\";\n");
    indent_line(doc, 1, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "  return \"aaa\"\n         \"bbb\";\n");
}

TEST_CASE("T3: wrapped ternary aligns ':' with its '?'") {
    Document doc("  x = c\n          ? aaa\n: bbb;\n");
    indent_line(doc, 2, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "  x = c\n          ? aaa\n          : bbb;\n");
}

TEST_CASE("T3: break after a nested open paren compounds continuation") {
    // Continuation stacks from the enclosing level, not anchor line + cont.
    Document doc("  if (check(setParam(\nx)))\n    ;\n");
    indent_line(doc, 1, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "  if (check(setParam(\n          x)))\n    ;\n");
}

TEST_CASE("T3: operator chain inside call arguments") {
    // The additive chain's fake paren sits at 'bb' plus one continuation.
    Document doc("  f(a, bb +\ncc);\n");
    indent_line(doc, 1, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "  f(a, bb +\n           cc);\n");
}

TEST_CASE("T3: member call chain and stream continuation") {
    Document doc("  return WithColor(OS)\n.get()\n<< \"e\";\n");
    indent_line(doc, 1, CppIndentStyle{});
    indent_line(doc, 2, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "  return WithColor(OS)\n             .get()\n         << \"e\";\n");
}

TEST_CASE("T3: braced init contents indent from the expression level") {
    Document doc("  struct S c = {\nX,\n  };\n");
    indent_line(doc, 1, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "  struct S c = {\n      X,\n  };\n");
}

TEST_CASE("statement continuation") {
    // AlignOperands: the wrapped operand aligns under the RHS start.
    CHECK(enter("int x = a +^\n") == "int x = a +\n        ^\n");
    CHECK(enter("foo(a,^ b);\n") == "foo(a,\n    ^ b);\n");

    CppIndentStyle dont_align;
    dont_align.align_operands = false;
    CHECK(enter("int x = a +^\n", dont_align) == "int x = a +\n    ^\n");
}

TEST_CASE("wrapped signature anchors the body on the declaration line") {
    // '{' on a continuation line must not shift the body or the '}'
    CHECK(enter("void long_name(int a,\n               int b) {^}\n") ==
          "void long_name(int a,\n               int b) {\n    ^\n}\n");
    // one step only: a braceless for's if-block anchors on the if line
    CHECK(enter("for (;;)\n    if (x) {^}\n") ==
          "for (;;)\n    if (x) {\n        ^\n    }\n");
}

TEST_CASE("open-bracket alignment") {
    // content after '(' on its line: wrapped arguments align with it
    CHECK(enter("int y = f(aa,^ bb);\n") == "int y = f(aa,\n          ^ bb);\n");
    // break directly after '(': plain continuation indent
    CHECK(enter("foo(^a);\n") == "foo(\n    ^a);\n");
    CppIndentStyle no_align;
    no_align.align_open_bracket = false;
    CHECK(enter("int y = f(aa,^ bb);\n", no_align) == "int y = f(aa,\n    ^ bb);\n");
}

TEST_CASE("braced list continuation style") {
    CppIndentStyle two;
    two.indent_width = 2;
    CHECK(enter("int arr[] = {^\n};\n", two) == "int arr[] = {\n  ^\n};\n");
    two.brace_init_continuation = true;
    CHECK(enter("int arr[] = {^\n};\n", two) == "int arr[] = {\n    ^\n};\n");
}

TEST_CASE("wrapped function declarator name") {
    CppIndentStyle llvm_names;
    llvm_names.indent_wrapped_function_names = false;
    // LLVM convention: after breaking behind the return type, the name
    // line stays at the declaration's indent
    Document doc("static std::map<int, int> &\n        foo::bar() {\n}\n");
    CHECK(indent_line(doc, 1, llvm_names).target_column == 0);
    // default style keeps continuation indent
    Document doc2("static std::map<int, int> &\nfoo::bar() {\n}\n");
    CHECK(indent_line(doc2, 1, CppIndentStyle{}).target_column == 4);
    // an assignment continuation is not a declarator name
    Document doc3("int x =\nf();\n");
    CHECK(indent_line(doc3, 1, llvm_names).target_column == 4);
}

TEST_CASE("comment lines and the original-column rule") {
    // aligned with the following case label: adopts its decision
    Document doc("switch (x) {\ncase 1:\n    a();\n// about case 2\ncase 2:\n    b();\n}\n");
    IndentDecision d = indent_line(doc, 3, CppIndentStyle{});
    CHECK(d.role == FormatRole::CaseLabel);
    CHECK(d.target_column == 0);
    // not aligned with what follows: keeps the case body indent
    Document doc2("switch (x) {\ncase 1:\n    a();\n        // stray\ncase 2:\n    b();\n}\n");
    CHECK(indent_line(doc2, 3, CppIndentStyle{}).target_column == 4);
    CHECK(doc2.snapshot().content() ==
          "switch (x) {\ncase 1:\n    a();\n    // stray\ncase 2:\n    b();\n}\n");
    // a comment at body indent before '}' stays at body indent
    Document doc3("void f() {\n    a();\n    // trailing\n}\n");
    indent_line(doc3, 2, CppIndentStyle{});
    CHECK(doc3.snapshot().content() == "void f() {\n    a();\n    // trailing\n}\n");
}

TEST_CASE("enumerators indent as siblings") {
    CHECK(enter("enum E {\n    A,^\n};\n") == "enum E {\n    A,\n    ^\n};\n");
}

TEST_CASE("macro body continuation uses the block indent width") {
    CppIndentStyle two;
    two.indent_width = 2; // continuation_indent stays 4
    Document doc("#define F(x) \\\nbody(x)\n");
    CHECK(indent_line(doc, 1, two).target_column == 2);
}

TEST_CASE("macro body call arguments align like code") {
    CppIndentStyle two;
    two.indent_width = 2;
    Document doc("#define F(a, b)   \\\n  impl((long)(a), \\\n(long)(b))\n");
    CHECK(indent_line(doc, 2, two).target_column == 7); // under '(long)(a)'
}

TEST_CASE("extern block follows namespace indent style") {
    CHECK(enter("extern \"C\" {^\nvoid f(int);\n}\n") ==
          "extern \"C\" {\n^\nvoid f(int);\n}\n");
    CppIndentStyle indented;
    indented.namespace_indentation = CppIndentStyle::NamespaceIndentation::All;
    CHECK(enter("extern \"C\" {^\nvoid f(int);\n}\n", indented) ==
          "extern \"C\" {\n    ^\nvoid f(int);\n}\n");
    // the #ifdef-guarded frame: declarations inside stay at column zero
    CHECK(enter("#ifdef __cplusplus\nextern \"C\" {\n#endif\nvoid f(int);^\n"
                "#ifdef __cplusplus\n}\n#endif\n") ==
          "#ifdef __cplusplus\nextern \"C\" {\n#endif\nvoid f(int);\n^\n"
          "#ifdef __cplusplus\n}\n#endif\n");
}

TEST_CASE("macro callback block indents as a block") {
    CHECK(enter("LLVM_DEBUG({\n    f();^\n});\n") == "LLVM_DEBUG({\n    f();\n    ^\n});\n");
}

TEST_CASE("statement after a goto label stays at block level") {
    CHECK(enter("void f() {\nout:^\n}\n") == "void f() {\nout:\n    ^\n}\n");
}

TEST_CASE("preprocessor directives stay at column zero") {
    CHECK(enter("void f() {\n    body();^\n}\n") == "void f() {\n    body();\n    ^\n}\n");
    Document doc("void f() {\n    #define X 1\n}\n");
    IndentDecision d = indent_line(doc, 1, CppIndentStyle{});
    CHECK(d.role == FormatRole::PreprocessorDirective);
    CHECK(doc.snapshot().content() == "void f() {\n#define X 1\n}\n");
}

TEST_CASE("unclosed macro invocation leaves distant indent stable") {
    // design.md §14.7 last sample: bounded degradation
    std::string fixture = "#define DECLARE(x) x\n\nDECLARE(\nnamespace foo {^\nint x;\n}\n";
    CHECK(enter(fixture) == "#define DECLARE(x) x\n\nDECLARE(\nnamespace foo {\n^\nint x;\n}\n");
}

TEST_CASE("raw strings and block comments are protected") {
    SUBCASE("explicit indent never touches raw string content") {
        std::string text = "auto s = R\"(\n  keep me\n)\";\n";
        Document doc(text);
        IndentDecision d = indent_line(doc, 1, CppIndentStyle{});
        CHECK(d.preserve);
        CHECK(d.role == FormatRole::PreservedRawString);
        CHECK(doc.snapshot().content() == text);
    }
    SUBCASE("enter inside a raw string inserts no indentation") {
        CHECK(enter("auto s = R\"(ab^cd)\";\n") == "auto s = R\"(ab\n^cd)\";\n");
    }
    SUBCASE("enter inside a block comment continues the previous indent") {
        CHECK(enter("void f() {\n    /* comment^ */\n}\n") ==
              "void f() {\n    /* comment\n    ^ */\n}\n");
    }
}

TEST_CASE("explicit indent only changes leading whitespace") {
    Document doc("namespace foo {\n        int x;   // trailing\n}\n");
    indent_line(doc, 1, CppIndentStyle{});
    CHECK(doc.snapshot().content() == "namespace foo {\nint x;   // trailing\n}\n");
}

TEST_CASE("smart tabs: structural part in tabs, alignment in spaces") {
    CppIndentStyle tabs;
    tabs.use_tabs = true;
    CHECK(enter("void f() {^}\n", tabs) == "void f() {\n\t^\n}\n");
    // ctor alignment beyond the structural level uses spaces
    auto [text, caret] = parse_caret("Foo::Foo()\n\t: a_(1),^\n{\n}\n");
    Document doc(std::move(text));
    EnterResult result = press_enter(doc, caret, tabs);
    CHECK(result.decision.role == FormatRole::ConstructorInitializerItem);
    // target column 6: base 0 (fn line) -> no full tab at width 4 from
    // structural 0, so all spaces
    CHECK(result.decision.indentation_text == std::string(6, ' '));
}

TEST_CASE("typed-char reindent") {
    SUBCASE("':' completing a case label dedents the line") {
        CHECK(type_chars("switch (x) {\n    ^\n}\n", "case 1:") ==
              "switch (x) {\ncase 1:^\n}\n");
    }
    SUBCASE("':' completing an access specifier") {
        CHECK(type_chars("class C {\n    int a;\n    ^\n};\n", "public:") ==
              "class C {\n    int a;\npublic:^\n};\n");
    }
    SUBCASE("':' opening a constructor initializer list") {
        CHECK(type_chars("Foo::Foo()\n^\n{\n}\n", ":") == "Foo::Foo()\n    :^\n{\n}\n");
    }
    SUBCASE("'}' as first content dedents to the opener's construct") {
        CHECK(type_chars("void f() {\n    body();\n    ^\n", "}") ==
              "void f() {\n    body();\n}^\n");
    }
    SUBCASE("'#' as first content snaps to column zero") {
        CHECK(type_chars("void f() {\n    ^\n}\n", "#") == "void f() {\n#^\n}\n");
    }
    SUBCASE("a ternary ':' is not a label") {
        CHECK(type_chars("    int x = a ? b ^;\n", ":") == "    int x = a ? b :^;\n");
    }
    SUBCASE("'}' after code on the line does not reindent") {
        CHECK(type_chars("void f() {\n    int a[] = {1^\n", "}") ==
              "void f() {\n    int a[] = {1}^\n");
    }
    SUBCASE("protected lines are never touched") {
        CHECK(type_chars("auto s = R\"(\nabc^\n)\";\n", ":") ==
              "auto s = R\"(\nabc:^\n)\";\n");
    }
}

TEST_CASE("typed-char reindent is one undo unit") {
    auto [text, caret] = parse_caret("switch (x) {\n    case 1^\n}\n");
    Document doc(std::move(text));
    TypeCharResult result = type_char(doc, caret, ':', CppIndentStyle{});
    CHECK(result.reindented);
    CHECK(doc.snapshot().content() == "switch (x) {\ncase 1:\n}\n");
    doc.undo();
    CHECK(doc.snapshot().content() == "switch (x) {\n    case 1\n}\n");
}

TEST_CASE("every decision carries a trace and a handler") {
    Document doc("void f() {}");
    EnterResult result = press_enter(doc, TextOffset{10}, CppIndentStyle{});
    REQUIRE(!result.decision.trace.empty());
    CHECK(result.decision.trace.front() == "enter handler: EnterBetweenBraces");
}

TEST_CASE("prefix typing: enter at every prefix is deterministic and safe") {
    static constexpr std::string_view kSample =
        "namespace foo {\n"
        "class Foo {\n"
        "public:\n"
        "    Foo();\n"
        "};\n"
        "Foo::Foo()\n"
        "    : a_(1),\n"
        "      b_{2}\n"
        "{\n"
        "    switch (x) {\n"
        "    case 1:\n"
        "        f([&] { return 1; });\n"
        "    default:\n"
        "        break;\n"
        "    }\n"
        "}\n"
        "}\n";
    for (std::size_t len = 0; len <= kSample.size(); ++len) {
        std::string prefix(kSample.substr(0, len));
        Document doc1(prefix);
        Document doc2(prefix);
        auto caret = TextOffset{static_cast<std::uint32_t>(doc1.snapshot().size_bytes())};
        EnterResult r1 = press_enter(doc1, caret, CppIndentStyle{});
        EnterResult r2 = press_enter(doc2, caret, CppIndentStyle{});
        REQUIRE(doc1.snapshot().content() == doc2.snapshot().content().to_string()); // deterministic
        REQUIRE(r1.caret == r2.caret);
        // the whole command stays one undo unit
        REQUIRE(doc1.undo().has_value());
        REQUIRE(doc1.snapshot().content() == prefix);
    }
}
