#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "syntax/structure.hpp"

#include <string>

using namespace cind;

namespace {

// '|' marks the query offset; removed before parsing.
struct Ctx {
    std::string text;
    TextOffset at;
    Text value;
    SyntaxTree tree;

    explicit Ctx(std::string_view marked) {
        auto bar = marked.find('|');
        REQUIRE(bar != std::string_view::npos);
        text = std::string(marked.substr(0, bar)) + std::string(marked.substr(bar + 1));
        at = TextOffset{static_cast<std::uint32_t>(bar)};
        value = Text(text);
        tree = parse(text);
    }

    std::string slice(TextRange range) const {
        return text.substr(range.start.value, range.length());
    }
};

} // namespace

TEST_CASE("sexp_forward: atoms, groups, blocking closers") {
    Ctx c("int f() { g(a, |h(b), c); }");
    auto unit = sexp_forward(c.tree, c.at);
    REQUIRE(unit);
    CHECK(c.slice(*unit) == "h");
    unit = sexp_forward(c.tree, unit->end);
    REQUIRE(unit);
    CHECK(c.slice(*unit) == "(b)"); // bracket pair is one unit

    Ctx blocked("int f() { g(a, h(b), c|); }");
    CHECK(!sexp_forward(blocked.tree, blocked.at)); // ')' of enclosing group

    Ctx mid("int f() { long_na|me + x; }");
    unit = sexp_forward(mid.tree, mid.at);
    REQUIRE(unit);
    CHECK(unit->end.value == mid.at.value + 2); // to the end of "long_name"

    Ctx tmpl("std::vector<std::pair<int, int>> |v;");
    // backward over the template group from just before 'v'
    auto back = sexp_backward(tmpl.tree, tmpl.at);
    REQUIRE(back);
    CHECK(c.slice(*back).empty() == false);
    CHECK(tmpl.slice(*back) == "<std::pair<int, int>>");
}

TEST_CASE("sexp_backward: groups collapse to one unit, openers block") {
    Ctx c("int f() { g(a, b)|; }");
    auto unit = sexp_backward(c.tree, c.at);
    REQUIRE(unit);
    CHECK(c.slice(*unit) == "(a, b)");
    unit = sexp_backward(c.tree, unit->start);
    REQUIRE(unit);
    CHECK(c.slice(*unit) == "g");

    Ctx blocked("int f() { g(|a, b); }");
    CHECK(!sexp_backward(blocked.tree, blocked.at));
}

TEST_CASE("enclosing_list and expand_selection") {
    Ctx c("int f() { g(a, |b + c); }");
    auto list = enclosing_list(c.tree, c.at);
    REQUIRE(list);
    CHECK(c.slice(*list) == "(a, b + c)");

    // expand: token -> group interior -> whole group -> ...
    auto sel = expand_selection(c.tree, TextRange{c.at, c.at});
    REQUIRE(sel);
    CHECK(c.slice(*sel) == "b");
    sel = expand_selection(c.tree, *sel);
    REQUIRE(sel);
    CHECK(c.slice(*sel) == "a, b + c");
    sel = expand_selection(c.tree, *sel);
    REQUIRE(sel);
    CHECK(c.slice(*sel) == "(a, b + c)");
    sel = expand_selection(c.tree, *sel);
    REQUIRE(sel);
    CHECK(c.slice(*sel).starts_with("g(a")); // statement / body level next
}

TEST_CASE("soft_kill_end keeps balance") {
    // Units starting past the line end stay; the enclosing ')' is never
    // crossed even though the statement continues.
    Ctx multi("int x = f(|a,\n          b);\nint y;");
    TextRange kill = soft_kill_end(multi.tree, multi.value, multi.at);
    CHECK(multi.slice(kill) == "a,");

    // Blocked immediately: nothing to kill inside the parens.
    Ctx blocked("int x = f(a|);");
    CHECK(soft_kill_end(blocked.tree, blocked.value, blocked.at).empty());

    // Plain end of line: the newline is killed.
    Ctx eol("int x;|\nint y;");
    kill = soft_kill_end(eol.tree, eol.value, eol.at);
    CHECK(eol.slice(kill) == "\n");

    // From line start: whole statement including trailing comment.
    Ctx line("|int x = 1; // note\nint y;");
    kill = soft_kill_end(line.tree, line.value, line.at);
    CHECK(line.slice(kill) == "int x = 1; // note");

    // A brace unit opened on this line swallows the whole block.
    Ctx block("void f() |{\n  a();\n}\nint z;");
    kill = soft_kill_end(block.tree, block.value, block.at);
    CHECK(block.slice(kill) == "{\n  a();\n}");
}
