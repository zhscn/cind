#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "syntax/syntax_tree.hpp"

#include <random>
#include <string>
#include <vector>

using namespace cind;

namespace {

void check_node(const SyntaxTree& tree, SyntaxNodeId id) {
    const SyntaxNode& n = tree.node(id);
    std::uint32_t cursor = n.first_token;
    for (SyntaxNodeId child_id : n.children) {
        const SyntaxNode& child = tree.node(child_id);
        REQUIRE(child.parent == id);
        REQUIRE(child.first_token >= cursor);
        REQUIRE(child.end_token >= child.first_token);
        REQUIRE(child.end_token <= n.end_token);
        cursor = child.end_token;
        check_node(tree, child_id);
    }
}

// design.md §14.3: terminates (implied), root covers everything, children
// are ordered/nested, no zero-length loops (progress guarantee).
void check_tree_invariants(const SyntaxTree& tree) {
    const SyntaxNode& root = tree.node(tree.root());
    REQUIRE(root.kind == SyntaxKind::TranslationUnit);
    REQUIRE(root.first_token == 0);
    REQUIRE(root.end_token == tree.tokens().size());
    check_node(tree, tree.root());
}

SyntaxTree parse_checked(std::string_view text) {
    SyntaxTree tree = parse(text);
    check_tree_invariants(tree);
    return tree;
}

std::vector<SyntaxNodeId> find_all(const SyntaxTree& tree, SyntaxKind kind) {
    std::vector<SyntaxNodeId> out;
    for (SyntaxNodeId id = 0; id < tree.node_count(); ++id) {
        if (tree.node(id).kind == kind) {
            out.push_back(id);
        }
    }
    return out;
}

SyntaxNodeId find_one(const SyntaxTree& tree, SyntaxKind kind) {
    auto all = find_all(tree, kind);
    REQUIRE(all.size() == 1);
    return all[0];
}

} // namespace

TEST_CASE("empty and trivial inputs") {
    parse_checked("");
    parse_checked("   \n\n  ");
    parse_checked("// only a comment\n");
}

TEST_CASE("namespace with declaration") {
    SyntaxTree tree = parse_checked("namespace foo {\nint x;\n}\n");
    SyntaxNodeId ns = find_one(tree, SyntaxKind::NamespaceDecl);
    SyntaxNodeId body = find_one(tree, SyntaxKind::NamespaceBody);
    CHECK(!tree.node(ns).incomplete);
    CHECK(!tree.node(body).incomplete);
    CHECK(tree.node(body).parent == ns);
    CHECK(find_all(tree, SyntaxKind::OpaqueDeclaration).size() == 1);
    CHECK(find_all(tree, SyntaxKind::MissingToken).empty());

    SUBCASE("unterminated body records a missing brace") {
        SyntaxTree open_tree = parse_checked("namespace foo {\nint x;\n");
        SyntaxNodeId open_body = find_one(open_tree, SyntaxKind::NamespaceBody);
        CHECK(open_tree.node(open_body).incomplete);
        SyntaxNodeId missing = find_one(open_tree, SyntaxKind::MissingToken);
        CHECK(open_tree.node(missing).expected == TokenKind::RBrace);
    }
}

TEST_CASE("class with access specifiers") {
    SyntaxTree tree = parse_checked("class Foo {\npublic:\n    Foo();\nprivate:\n    int x_;\n};\n");
    find_one(tree, SyntaxKind::ClassDecl);
    SyntaxNodeId body = find_one(tree, SyntaxKind::ClassBody);
    CHECK(!tree.node(body).incomplete);
    CHECK(find_all(tree, SyntaxKind::AccessSpecifierLabel).size() == 2);
}

TEST_CASE("switch with case sections") {
    SyntaxTree tree = parse_checked(
        "switch (x) {\ncase 1:\n    foo();\n    bar();\ndefault:\n    break;\n}\n");
    find_one(tree, SyntaxKind::SwitchStatement);
    auto sections = find_all(tree, SyntaxKind::CaseSection);
    REQUIRE(sections.size() == 2);
    // the two statements belong to the first section
    const SyntaxNode& first = tree.node(sections[0]);
    std::size_t stmt_children = 0;
    for (SyntaxNodeId c : first.children) {
        if (tree.node(c).kind == SyntaxKind::OpaqueDeclaration) {
            ++stmt_children;
        }
    }
    CHECK(stmt_children == 2);
}

TEST_CASE("constructor with initializer list") {
    SyntaxTree tree = parse_checked("Foo::Foo()\n    : a_(1),\n      b_{2}\n{\n}\n");
    SyntaxNodeId fn = find_one(tree, SyntaxKind::FunctionDefinition);
    SyntaxNodeId list = find_one(tree, SyntaxKind::CtorInitializerList);
    CHECK(!tree.node(list).incomplete);
    CHECK(!tree.node(fn).incomplete);
    CHECK(find_all(tree, SyntaxKind::CtorInitializer).size() == 2);
    find_one(tree, SyntaxKind::BraceGroup); // b_{2}
    find_one(tree, SyntaxKind::CompoundStatement);
}

TEST_CASE("bare colon after constructor declarator") {
    SyntaxTree tree = parse_checked("Foo::Foo()\n    :\n");
    SyntaxNodeId fn = find_one(tree, SyntaxKind::FunctionDefinition);
    SyntaxNodeId list = find_one(tree, SyntaxKind::CtorInitializerList);
    CHECK(tree.node(list).incomplete);
    CHECK(tree.node(fn).incomplete);
    // body brace is recorded as missing
    bool missing_lbrace = false;
    for (SyntaxNodeId m : find_all(tree, SyntaxKind::MissingToken)) {
        missing_lbrace |= tree.node(m).expected == TokenKind::LBrace;
    }
    CHECK(missing_lbrace);
}

TEST_CASE("declaration-prefix macro does not break the constructor") {
    SyntaxTree tree = parse_checked("MY_API\nFoo::Foo()\n    : a_(1)\n{\n}\n");
    find_one(tree, SyntaxKind::FunctionDefinition);
    SyntaxNodeId list = find_one(tree, SyntaxKind::CtorInitializerList);
    CHECK(!tree.node(list).incomplete);
}

TEST_CASE("unclosed macro invocation has bounded damage") {
    SyntaxTree tree =
        parse_checked("#define DECLARE(x) x\n\nDECLARE(\nnamespace foo {\nint x;\n}\n");
    SyntaxNodeId pp = find_one(tree, SyntaxKind::PreprocessorDirective);
    // two groups: '(x)' inside the #define, and the unclosed 'DECLARE(',
    // which bailed at 'namespace' instead of swallowing it
    auto groups = find_all(tree, SyntaxKind::ParenGroup);
    REQUIRE(groups.size() == 2);
    CHECK(tree.node(groups[0]).parent == pp);
    CHECK(!tree.node(groups[0]).incomplete);
    CHECK(tree.node(groups[1]).incomplete);
    SyntaxNodeId ns = find_one(tree, SyntaxKind::NamespaceDecl);
    SyntaxNodeId body = find_one(tree, SyntaxKind::NamespaceBody);
    CHECK(!tree.node(ns).incomplete);
    CHECK(!tree.node(body).incomplete);
}

TEST_CASE("if without braces") {
    SyntaxTree tree = parse_checked("if (x)\n    foo();\nbar();\n");
    SyntaxNodeId if_node = find_one(tree, SyntaxKind::IfStatement);
    CHECK(!tree.node(if_node).incomplete);
    // foo(); is inside the if, bar(); is a sibling after it
    TextRange if_range = tree.node_range(if_node);
    std::string_view text = "if (x)\n    foo();\nbar();\n";
    CHECK(text.substr(0, if_range.end.value).ends_with("foo();"));

    SUBCASE("missing body marks the statement incomplete") {
        SyntaxTree open_tree = parse_checked("if (x)\n");
        CHECK(open_tree.node(find_one(open_tree, SyntaxKind::IfStatement)).incomplete);
    }
    SUBCASE("else if chains nest") {
        SyntaxTree chain = parse_checked("if (a)\n    f();\nelse if (b)\n    g();\nelse\n    h();\n");
        CHECK(find_all(chain, SyntaxKind::IfStatement).size() == 2);
        CHECK(find_all(chain, SyntaxKind::ElseClause).size() == 2);
    }
}

TEST_CASE("lambda body inside an argument list") {
    SyntaxTree tree = parse_checked("foo([&](int x) {\n    return x;\n});\n");
    // one compound statement: the lambda body, nested inside foo's ParenGroup
    SyntaxNodeId body = find_one(tree, SyntaxKind::CompoundStatement);
    auto groups = find_all(tree, SyntaxKind::ParenGroup);
    CHECK(groups.size() == 2); // foo(...) and (int x)
    // body's ancestors include a ParenGroup
    bool inside_group = false;
    for (SyntaxNodeId p = tree.node(body).parent; p != kInvalidNode; p = tree.node(p).parent) {
        inside_group |= tree.node(p).kind == SyntaxKind::ParenGroup;
    }
    CHECK(inside_group);
}

TEST_CASE("lambda assigned to a variable gets a body block") {
    SyntaxTree tree = parse_checked("auto f = [](int x) {\n    return x;\n};\n");
    find_one(tree, SyntaxKind::CompoundStatement);
}

TEST_CASE("template angle heuristic") {
    SUBCASE("declaration position pairs") {
        SyntaxTree tree = parse_checked("std::vector<std::vector<int>> v;\n");
        find_one(tree, SyntaxKind::TemplateArgumentList);
    }
    SUBCASE("template prefix") {
        SyntaxTree tree = parse_checked("template <typename T>\nclass Foo {\n};\n");
        find_one(tree, SyntaxKind::TemplateArgumentList);
        find_one(tree, SyntaxKind::ClassBody);
    }
    SUBCASE("comparison does not pair") {
        SyntaxTree tree = parse_checked("bool b = a < b;\n");
        CHECK(find_all(tree, SyntaxKind::TemplateArgumentList).empty());
    }
    SUBCASE("unclosed angle degrades to an operator") {
        SyntaxTree tree = parse_checked("foo<bar baz;\nint x;\n");
        CHECK(find_all(tree, SyntaxKind::TemplateArgumentList).empty());
        CHECK(find_all(tree, SyntaxKind::OpaqueDeclaration).size() == 2);
    }
}

TEST_CASE("split-brace #if/#else branches parse as sibling alternatives") {
    // #ifdef A opens a brace in one branch, #else opens another, and a single
    // '}' after #endif closes it. clang-format's model treats the branches as
    // alternatives (one net brace), so f and g are siblings, not g nested
    // inside f's unclosed body. design.md §276.
    SyntaxTree tree = parse_checked(
        "#ifdef A\nvoid f() {\n#else\nvoid g() {\n#endif\n    body();\n}\n");
    CHECK(find_all(tree, SyntaxKind::PreprocessorDirective).size() == 3);
    auto fns = find_all(tree, SyntaxKind::FunctionDefinition);
    REQUIRE(fns.size() == 2); // ids are DFS-preorder: fns[0] == f, fns[1] == g
    CHECK(tree.node(fns[0]).parent == tree.root());
    CHECK(tree.node(fns[1]).parent == tree.root());
    // f's body is force-closed at #else (no real '}'); the shared body and the
    // closing brace attach to the last branch (g), which closes cleanly.
    SyntaxNodeId f_body = tree.node(fns[0]).children.back();
    SyntaxNodeId g_body = tree.node(fns[1]).children.back();
    CHECK(tree.node(f_body).kind == SyntaxKind::CompoundStatement);
    CHECK(tree.node(g_body).kind == SyntaxKind::CompoundStatement);
    CHECK(tree.node(f_body).incomplete);
    CHECK(!tree.node(g_body).incomplete);
}

TEST_CASE("split-brace #if inside a function keeps the tail at body level") {
    // The raw_socket_stream.cpp cascade: two `if (...) {` alternatives share
    // one closing brace. The bug nested if2 inside if1, pushing everything
    // after #endif one level too deep and cascading past the function.
    SyntaxTree tree = parse_checked("int h() {\n"
                                    "#ifdef _WIN32\n"
                                    "  if (a) {\n"
                                    "#else\n"
                                    "  if (b) {\n"
                                    "#endif\n"
                                    "    inner();\n"
                                    "  }\n"
                                    "  tail();\n"
                                    "}\n");
    auto ifs = find_all(tree, SyntaxKind::IfStatement);
    REQUIRE(ifs.size() == 2);
    // both ifs are siblings in the same (function) compound, not nested
    const SyntaxNodeId scope = tree.node(ifs[0]).parent;
    CHECK(tree.node(ifs[1]).parent == scope);
    CHECK(tree.node(scope).kind == SyntaxKind::CompoundStatement);
    // tail() lives directly in the function body — the cascade is gone
    bool tail_at_body = false;
    for (SyntaxNodeId child : tree.node(scope).children) {
        if (tree.node(child).kind == SyntaxKind::OpaqueDeclaration) {
            tail_at_body = true; // an opaque stmt (inner()/tail()) at body level
        }
    }
    CHECK(tail_at_body);
    // the function body closes cleanly (its own '}'), nothing left dangling
    SyntaxNodeId fn = find_one(tree, SyntaxKind::FunctionDefinition);
    CHECK(!tree.node(fn).incomplete);
}

TEST_CASE("declarator suffixes still introduce a function body") {
    SUBCASE(") const {") {
        SyntaxTree tree = parse_checked(
            "class C {\n    int f(int a) const { return a; }\n    void g() {}\n};\n");
        auto fns = find_all(tree, SyntaxKind::FunctionDefinition);
        CHECK(fns.size() == 2);
        // both stay members: the class body is not closed early
        SyntaxNodeId body = find_one(tree, SyntaxKind::ClassBody);
        for (SyntaxNodeId fn : fns) {
            CHECK(tree.node(fn).parent == body);
        }
        CHECK(find_all(tree, SyntaxKind::MissingToken).empty());
    }
    SUBCASE("trailing return type") {
        SyntaxTree tree = parse_checked("auto f(int a) -> int * { return &a; }\n");
        find_one(tree, SyntaxKind::FunctionDefinition);
        find_one(tree, SyntaxKind::CompoundStatement);
        CHECK(find_all(tree, SyntaxKind::BraceGroup).empty());
    }
    SUBCASE("noexcept constructor initializer") {
        SyntaxTree tree = parse_checked("X::X() noexcept : a_(1) {}\n");
        find_one(tree, SyntaxKind::CtorInitializerList);
        find_one(tree, SyntaxKind::FunctionDefinition);
    }
}

TEST_CASE("trailing-return lambda inside an argument list") {
    SyntaxTree tree = parse_checked("E = handleErrors(std::move(E), [&](const X &E) -> Error {\n"
                                    "    use(E);\n    return f();\n});\n");
    find_one(tree, SyntaxKind::CompoundStatement);
    CHECK(find_all(tree, SyntaxKind::BraceGroup).empty());
    CHECK(find_all(tree, SyntaxKind::MissingToken).empty());
}

TEST_CASE("statement evidence upgrades a brace group to a block") {
    // macro callback: LLVM_DEBUG({ statements; });
    SyntaxTree tree = parse_checked("LLVM_DEBUG({\n    dbgs() << x;\n    f();\n});\n");
    find_one(tree, SyntaxKind::CompoundStatement);
    CHECK(find_all(tree, SyntaxKind::MissingToken).empty());
}

TEST_CASE("brace after '=' in a struct-typed variable is an init list") {
    SyntaxTree tree = parse_checked(
        "static const struct f_cnvrt Convert = {\n    SETCVTOFF, // cvtcmd\n"
        "    0,         // pccsid\n};\n");
    CHECK(find_all(tree, SyntaxKind::ClassBody).empty());
    find_one(tree, SyntaxKind::BraceGroup);
    CHECK(find_all(tree, SyntaxKind::MissingToken).empty());
}

TEST_CASE("capture-only lambda gets a body block") {
    SyntaxTree tree = parse_checked("auto f = [this] { return go(); };\n");
    find_one(tree, SyntaxKind::CompoundStatement);
    CHECK(find_all(tree, SyntaxKind::MissingToken).empty());
}

TEST_CASE("goto label is complete on its own") {
    SyntaxTree tree = parse_checked("void f() {\nretry:\n    if (x)\n        goto retry;\n}\n");
    find_one(tree, SyntaxKind::IfStatement); // sibling of the label, not swallowed
    SyntaxNodeId fn = find_one(tree, SyntaxKind::FunctionDefinition);
    CHECK(!tree.node(fn).incomplete);
    // in-class constructor initializer is not a label
    SyntaxTree tree2 = parse_checked("struct S {\n    S() : a_(1) {}\n};\n");
    find_one(tree2, SyntaxKind::CtorInitializerList);
}

TEST_CASE("macro bodies get group structure, bounded to the directive") {
    SyntaxTree tree = parse_checked(
        "#define F(a, b) \\\n  impl((long)(a), \\\n       (long)(b))\nint x;\n");
    CHECK(find_all(tree, SyntaxKind::PreprocessorDirective).size() == 1);
    CHECK(find_all(tree, SyntaxKind::ParenGroup).size() >= 2);
    // unbalanced parens inside a macro never leak past the line
    SyntaxTree tree2 = parse_checked("#define LPAREN (\nint y;\n");
    SyntaxNodeId pp = find_one(tree2, SyntaxKind::PreprocessorDirective);
    SyntaxNodeId group = find_one(tree2, SyntaxKind::ParenGroup);
    CHECK(tree2.node(group).parent == pp);
    CHECK(find_all(tree2, SyntaxKind::OpaqueDeclaration).size() == 1); // int y;
}

TEST_CASE("extern linkage block has namespace semantics") {
    SyntaxTree tree =
        parse_checked("extern \"C\" {\nvoid f(int);\nint g(void) { return 0; }\n}\n");
    SyntaxNodeId body = find_one(tree, SyntaxKind::NamespaceBody);
    CHECK(tree.node(body).children.size() >= 2);
    CHECK(find_all(tree, SyntaxKind::MissingToken).empty());
    // a plain extern declaration is untouched
    SyntaxTree tree2 = parse_checked("extern \"C\" void f(int);\nextern int x;\n");
    CHECK(find_all(tree2, SyntaxKind::NamespaceBody).empty());
}

TEST_CASE("ifdef-guarded extern block pairs across the branches") {
    // The canonical C header frame: '{' and '}' live in different #if
    // branches; all branches parse active, so the pair still matches.
    SyntaxTree tree = parse_checked("#ifdef __cplusplus\n"
                                    "extern \"C\" {\n"
                                    "#endif\n"
                                    "\n"
                                    "void f(int);\n"
                                    "int g(void);\n"
                                    "\n"
                                    "#ifdef __cplusplus\n"
                                    "}\n"
                                    "#endif\n");
    SyntaxNodeId body = find_one(tree, SyntaxKind::NamespaceBody);
    CHECK(!tree.node(body).incomplete);
    CHECK(find_all(tree, SyntaxKind::MissingToken).empty());
    CHECK(find_all(tree, SyntaxKind::Error).empty());
    // the declarations are children of the linkage body
    std::size_t decls = 0;
    for (SyntaxNodeId child : tree.node(body).children) {
        decls += tree.node(child).kind == SyntaxKind::OpaqueDeclaration ? 1 : 0;
    }
    CHECK(decls == 2);
}

TEST_CASE("enumerators are sibling declarations, not one continuation") {
    SyntaxTree tree = parse_checked("enum E {\n    A,\n    B = f(1, 2),\n    C\n};\n");
    SyntaxNodeId body = find_one(tree, SyntaxKind::ClassBody);
    CHECK(tree.node(body).children.size() == 3);
    for (SyntaxNodeId child : tree.node(body).children) {
        CHECK(!tree.node(child).incomplete);
    }
}

TEST_CASE("= default is not a case label") {
    SyntaxTree tree = parse_checked("X::X() = default;\nvoid f() {}\n");
    CHECK(find_all(tree, SyntaxKind::CaseSection).empty());
    find_one(tree, SyntaxKind::FunctionDefinition);
}

TEST_CASE("macro invocation without a semicolon ends at the line break") {
    SyntaxTree tree = parse_checked("DEFINE_THING(Kernel::Metadata)\nvoid f() {\n    g();\n}\n");
    SyntaxNodeId fn = find_one(tree, SyntaxKind::FunctionDefinition);
    CHECK(tree.node(fn).parent == tree.root());
    // the macro line is a complete sibling, not a swallowing parent
    SyntaxNodeId macro = tree.node(tree.root()).children.front();
    CHECK(tree.node(macro).kind == SyntaxKind::OpaqueDeclaration);
    CHECK(!tree.node(macro).incomplete);
}

TEST_CASE("do while") {
    SyntaxTree tree = parse_checked("do {\n    f();\n} while (x);\n");
    SyntaxNodeId n = find_one(tree, SyntaxKind::DoStatement);
    CHECK(!tree.node(n).incomplete);
}

TEST_CASE("node_at finds the deepest node") {
    std::string text = "namespace foo {\nint x;\n}\n";
    SyntaxTree tree = parse_checked(text);
    // offset of 'x'
    TextOffset x_pos{static_cast<std::uint32_t>(text.find('x'))};
    SyntaxNodeId at_x = tree.node_at(x_pos);
    CHECK(tree.node(at_x).kind == SyntaxKind::OpaqueDeclaration);
}

TEST_CASE("every prefix of a realistic file parses with invariants intact") {
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
        "    if (y)\n"
        "        g();\n"
        "}\n"
        "}\n";
    for (std::size_t len = 0; len <= kSample.size(); ++len) {
        parse_checked(kSample.substr(0, len));
    }
}

TEST_CASE("fuzz: arbitrary input never breaks parser invariants") {
    std::mt19937 rng(424242);
    std::uniform_int_distribution<int> len_dist(0, 300);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> mode_dist(0, 3);
    static constexpr std::string_view kCppish =
        " \t\nabcxR\"'(){}[]<>:;,#\\/*=+-.0123456789_"
        "namespace class if else for case default public template operator";

    for (int round = 0; round < 300; ++round) {
        std::string text;
        const int len = len_dist(rng);
        const int mode = mode_dist(rng);
        for (int i = 0; i < len; ++i) {
            if (mode == 0) {
                text.push_back(static_cast<char>(byte_dist(rng)));
            } else {
                text.push_back(
                    kCppish[static_cast<std::size_t>(byte_dist(rng)) % kCppish.size()]);
            }
        }
        if (text.find('\r') != std::string::npos) {
            continue;
        }
        parse_checked(text);
    }
}

// design.md §14: structural churn — how many levels of the caret's ancestor
// path change per typed character. Error recovery's goal is edit stability:
// prefixes should converge to a stable spine early and stay there, not flip
// between label/expression/error readings.
namespace {

std::vector<SyntaxKind> ancestor_path_at_end(const SyntaxTree& tree, std::size_t size) {
    const TextOffset probe{size > 0 ? static_cast<std::uint32_t>(size - 1) : 0};
    std::vector<SyntaxKind> path;
    for (SyntaxNodeId id = tree.node_at(probe); id != kInvalidNode;
         id = tree.node(id).parent) {
        path.push_back(tree.node(id).kind);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// Sum over keystrokes of levels dropped + levels gained relative to the
// previous prefix's ancestor path.
int total_churn(std::string_view sample) {
    std::vector<SyntaxKind> previous;
    int total = 0;
    for (std::size_t i = 1; i <= sample.size(); ++i) {
        SyntaxTree tree = parse(sample.substr(0, i));
        std::vector<SyntaxKind> path = ancestor_path_at_end(tree, i);
        std::size_t common = 0;
        while (common < previous.size() && common < path.size() &&
               previous[common] == path[common]) {
            ++common;
        }
        total += static_cast<int>(previous.size() - common) +
                 static_cast<int>(path.size() - common);
        previous = std::move(path);
    }
    return total;
}

} // namespace

TEST_CASE("structural churn stays bounded while typing (golden)") {
    // Constructor with initializer list: the design's canonical stability
    // sample (§14). The spine must converge to FunctionDefinition +
    // initializer list early and not flip per keystroke.
    // Measured 21/25 when locked in; the bound is a regression tripwire, not
    // a target — recovery changes that raise it need a conscious decision.
    const std::string_view ctor = "Foo::Foo() : a_(1), b_(2) {}";
    const int ctor_churn = total_churn(ctor);
    MESSAGE("ctor churn: ", ctor_churn);
    CHECK(ctor_churn <= 24);

    const std::string_view stmt = "if (a && b(c))\n    return d + e;\n";
    const int stmt_churn = total_churn(stmt);
    MESSAGE("stmt churn: ", stmt_churn);
    CHECK(stmt_churn <= 28);
}
