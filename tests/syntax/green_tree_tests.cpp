#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "syntax/green_node.hpp"
#include "syntax/syntax_tree.hpp"

#include <string_view>

using namespace cind;

namespace {

// The lazily materialized red pool must mirror the green tree exactly: matching
// kind/flags/expected, absolute token span reconstructed from the relative
// leadings/widths, parent links, and child structure — recursively. Holding a
// red reference across the recursive t.node() calls also exercises the pool's
// stable-address guarantee.
void check_red_matches_green(const SyntaxTree& t, SyntaxNodeId id, const GreenNode* g,
                             std::uint32_t base) {
    const SyntaxNode& n = t.node(id);
    REQUIRE(n.kind == g->kind);
    REQUIRE(n.first_token == base);
    REQUIRE(n.end_token == base + g->width);
    REQUIRE(n.incomplete == g->incomplete);
    REQUIRE(n.reclassified == g->reclassified);
    REQUIRE(n.expected == g->expected);
    REQUIRE(n.children.size() == g->children.size());
    std::uint32_t cursor = base;
    for (std::size_t k = 0; k < g->children.size(); ++k) {
        const std::uint32_t cf = cursor + g->children[k].leading;
        const SyntaxNodeId cid = n.children[k];
        REQUIRE(t.node(cid).parent == id);
        check_red_matches_green(t, cid, g->children[k].node.get(), cf);
        cursor = cf + g->children[k].node->width;
    }
}

void roundtrip(std::string_view src) {
    SyntaxTree t = parse(src);
    REQUIRE(t.green_root() != nullptr);
    check_red_matches_green(t, t.root(), t.green_root().get(), 0);
    REQUIRE(t.node_count() == green_count(t.green_root()));
    REQUIRE(green_equal(t.green_root(), t.green_root()));
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
