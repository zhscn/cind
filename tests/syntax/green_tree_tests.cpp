#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "syntax/green_node.hpp"
#include "syntax/syntax_tree.hpp"

#include <string_view>

using namespace cind;

namespace {

// flat -> green -> flat must reproduce the tree byte-exact: same DFS-preorder
// ids, same per-node fields, same dump. This is the Phase-1 encoding proof.
void roundtrip(std::string_view src) {
    SyntaxTree t = parse(src);
    SyntaxTree back = flat_from_green(green_from_flat(t), t.tokens());

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
    REQUIRE(back.dump(src) == t.dump(src));
}

} // namespace

TEST_CASE("green round-trip: empty and whitespace-only") {
    roundtrip("");
    roundtrip("\n");
    roundtrip("   \n\t\n");
    roundtrip("// only a comment\n");
    roundtrip("/* block */\n");
}

TEST_CASE("green round-trip: root owns trailing trivia and EOF") {
    roundtrip("int x;\n\n// trailing\n");
    roundtrip("int x;   ");
    roundtrip("void f() {}\n\n\n");
}

TEST_CASE("green round-trip: zero-width MissingToken nodes") {
    roundtrip("int f(");             // missing ')'
    roundtrip("void g() {");         // missing '}'
    roundtrip("struct S {");         // missing '}' and ';'
    roundtrip("if (x)");             // incomplete if
    roundtrip("foo(a, b");           // missing ')'
}

TEST_CASE("green round-trip: children not covering full width") {
    roundtrip("struct S {};\n");     // ClassDecl owns trailing ';' after ClassBody
    roundtrip("class C { int x; };\n");
    roundtrip("namespace n { int y; }\n"); // NamespaceDecl owns 'namespace n' prefix
    roundtrip("enum E { A, B, C };\n");
}

TEST_CASE("green round-trip: reclassified brace group") {
    roundtrip("LLVM_DEBUG({ int x = 1; return; });\n"); // BraceGroup -> CompoundStatement
    roundtrip("auto f = [] { return 1; };\n");
}

TEST_CASE("green round-trip: Error nodes from junk") {
    roundtrip("@@@ ### $$$\n");
    roundtrip("int 123 = ;;;\n");
    roundtrip(")))}}}\n");
}

TEST_CASE("green round-trip: preprocessor and conditionals") {
    roundtrip("#ifndef H\n#define H\nint x;\n#endif\n");
    roundtrip("#ifdef _WIN32\n  if (a) {\n#else\n  if (b) {\n#endif\n    return;\n  }\n");
    roundtrip("extern \"C\" {\nint c(void);\n}\n");
}

TEST_CASE("green round-trip: a realistic function") {
    roundtrip(R"cpp(
namespace ns {
class Widget : public Base {
public:
    Widget(int a, int b) : a_(a), b_(b) {}
    int compute() const {
        for (int i = 0; i < a_; ++i) {
            switch (b_) {
            case 0:
                return i;
            default:
                break;
            }
        }
        return -1;
    }
private:
    int a_, b_;
};
} // namespace ns
)cpp");
}
