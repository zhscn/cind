#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "formatting/clang_format_style.hpp"

#include <cstdio>
#include <string>

using namespace cind;
using NI = CppIndentStyle::NamespaceIndentation;

TEST_CASE("LLVM preset maps clang-format defaults") {
    CppIndentStyle style;
    REQUIRE(apply_clang_format_preset("LLVM", style));
    CHECK(style.indent_width == 2);
    CHECK(style.continuation_indent == 4);
    CHECK(style.tab_width == 8);
    CHECK(style.use_tabs == false);
    // AccessModifierOffset -2 relative to member indent -> flush with 'class'.
    CHECK(style.access_specifier_offset == 0);
    CHECK(style.namespace_indentation == NI::None);
    CHECK(style.indent_case_label == false);
    CHECK(style.brace_init_continuation == true);
    CHECK(style.align_open_bracket == true);
    CHECK(style.indent_wrapped_function_names == false);
}

TEST_CASE("preset names are case-insensitive, unknown names refused") {
    CppIndentStyle a, b;
    REQUIRE(apply_clang_format_preset("llvm", a));
    REQUIRE(apply_clang_format_preset("LLVM", b));
    CHECK(a == b);

    CppIndentStyle untouched;
    untouched.indent_width = 7;
    CppIndentStyle probe = untouched;
    CHECK(!apply_clang_format_preset("NotAStyle", probe));
    CHECK(probe == untouched);
}

TEST_CASE("WebKit preset: Inner namespaces, no bracket alignment") {
    CppIndentStyle style;
    REQUIRE(apply_clang_format_preset("WebKit", style));
    CHECK(style.indent_width == 4);
    CHECK(style.namespace_indentation == NI::Inner);
    CHECK(style.align_open_bracket == false);
    CHECK(style.brace_init_continuation == false);
    CHECK(style.access_specifier_offset == 0); // 4 + (-4)
}

TEST_CASE("access modifier offset converts against the final IndentWidth") {
    // clang-format: offset is relative to the member indent, so overriding
    // IndentWidth after BasedOnStyle moves the access specifier too.
    auto result = parse_clang_format_yaml("BasedOnStyle: LLVM\nIndentWidth: 4\n", {});
    CHECK(result.style.indent_width == 4);
    CHECK(result.style.access_specifier_offset == 2); // 4 + (-2)
    CHECK(result.warnings.empty());
}

TEST_CASE("keys apply on top of the base style without BasedOnStyle") {
    CppIndentStyle base;
    base.indent_width = 3;
    auto result = parse_clang_format_yaml("ContinuationIndentWidth: 6\n", base);
    CHECK(result.style.indent_width == 3);
    CHECK(result.style.continuation_indent == 6);
}

TEST_CASE("old and new enum spellings are both accepted") {
    CppIndentStyle base;
    CHECK(parse_clang_format_yaml("Cpp11BracedListStyle: true\n", base)
              .style.brace_init_continuation == true);
    CHECK(parse_clang_format_yaml("Cpp11BracedListStyle: AlignFirstComment\n", base)
              .style.brace_init_continuation == true);
    CHECK(parse_clang_format_yaml("Cpp11BracedListStyle: Block\n", base)
              .style.brace_init_continuation == false);
    CHECK(parse_clang_format_yaml("AlignAfterOpenBracket: DontAlign\n", base)
              .style.align_open_bracket == false);
    CHECK(parse_clang_format_yaml("AlignAfterOpenBracket: AlwaysBreak\n", base)
              .style.align_open_bracket == false);
    CHECK(parse_clang_format_yaml("AlignAfterOpenBracket: true\n", base)
              .style.align_open_bracket == true);
    CHECK(parse_clang_format_yaml("UseTab: ForIndentation\n", base).style.use_tabs == true);
    CHECK(parse_clang_format_yaml("NamespaceIndentation: Inner\n", base)
              .style.namespace_indentation == NI::Inner);
}

TEST_CASE("multi-document files select the Cpp document") {
    const char* yaml =
        "BasedOnStyle: LLVM\n"
        "---\n"
        "Language: JavaScript\n"
        "IndentWidth: 8\n"
        "---\n"
        "Language: Cpp\n"
        "IndentWidth: 4\n"
        "...\n";
    auto result = parse_clang_format_yaml(yaml, {});
    CHECK(result.style.indent_width == 4);        // Cpp doc, not the JS one
    CHECK(result.style.continuation_indent == 4); // from the common LLVM doc
}

TEST_CASE("nested blocks, comments and quoted values") {
    const char* yaml =
        "# a header comment\n"
        "IndentWidth: 4 # trailing comment\n"
        "BraceWrapping:\n"
        "  AfterClass: true\n"
        "  IndentBraces: true\n"
        "UseTab: \"Never\"\n";
    auto result = parse_clang_format_yaml(yaml, {});
    CHECK(result.style.indent_width == 4);
    CHECK(result.style.use_tabs == false);
    CHECK(result.warnings.empty());
}

TEST_CASE("unsupported indentation keys produce warnings, not silence") {
    CppIndentStyle base;
    CHECK(parse_clang_format_yaml("IndentPPDirectives: BeforeHash\n", base).warnings.size() == 1);
    CHECK(parse_clang_format_yaml("IndentPPDirectives: None\n", base).warnings.empty());
    CHECK(parse_clang_format_yaml("IndentAccessModifiers: true\n", base).warnings.size() == 1);
    CHECK(parse_clang_format_yaml("LambdaBodyIndentation: OuterScope\n", base).warnings.size() ==
          1);
    CHECK(parse_clang_format_yaml("IndentExternBlock: Indent\n", base).warnings.size() == 1);
    CHECK(parse_clang_format_yaml("BasedOnStyle: NoSuchStyle\n", base).warnings.size() == 1);
    CHECK(parse_clang_format_yaml("IndentWidth: banana\n", base).warnings.size() == 1);
    // ConstructorInitializerIndentWidth differing from continuation width is
    // the one mapping we cannot honor exactly.
    auto ctor = parse_clang_format_yaml(
        "BasedOnStyle: LLVM\nConstructorInitializerIndentWidth: 8\n", base);
    CHECK(ctor.warnings.size() == 1);
    // Unrelated formatter keys stay silent.
    CHECK(parse_clang_format_yaml("SortIncludes: Never\nColumnLimit: 120\n", base)
              .warnings.empty());
}

TEST_CASE("DisableFormat is surfaced to the caller") {
    CHECK(parse_clang_format_yaml("DisableFormat: true\nSortIncludes: Never\n", {})
              .disable_format);
    CHECK(!parse_clang_format_yaml("DisableFormat: false\n", {}).disable_format);
}

TEST_CASE("InheritParentConfig is reported to the caller") {
    auto result = parse_clang_format_yaml("BasedOnStyle: InheritParentConfig\nIndentWidth: 2\n",
                                          {});
    CHECK(result.inherit_parent);
    CHECK(result.style.indent_width == 2);
}

// Feed each preset's actual `clang-format --dump-config` output through the
// parser and require it to reproduce our built-in table. This validates the
// preset values against the real tool and exercises the parser on a full
// ~200-key document (nested blocks, sequences, comments). Skips when
// clang-format is not installed.
TEST_CASE("preset table cross-validates against clang-format --dump-config") {
    for (const char* name :
         {"LLVM", "Google", "Chromium", "Mozilla", "WebKit", "GNU", "Microsoft"}) {
        const std::string cmd = std::string("clang-format --style=") + name +
                                " --dump-config 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        REQUIRE(pipe != nullptr);
        std::string dump;
        char buf[1 << 16];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, pipe)) > 0) {
            dump.append(buf, n);
        }
        if (pclose(pipe) != 0 || dump.empty()) {
            MESSAGE("clang-format not available; skipping cross-validation");
            return;
        }

        CAPTURE(name);
        ClangFormatStyle parsed = parse_clang_format_yaml(dump, {});
        CHECK(parsed.warnings.empty());
        CppIndentStyle expected;
        REQUIRE(apply_clang_format_preset(name, expected));
        CHECK(parsed.style == expected);
    }
}
