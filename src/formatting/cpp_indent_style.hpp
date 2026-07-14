#pragma once

namespace cind {

struct CppIndentStyle {
    int indent_width = 4;
    int continuation_indent = 4;
    int tab_width = 4;
    bool use_tabs = false;

    // T2.5: when the open bracket of a group has content after it on its
    // line, wrapped lines align with that content (clang-format
    // AlignAfterOpenBracket, CLion "align when multiline").
    bool align_open_bracket = true;
    // Braced-list contents use continuation_indent instead of indent_width
    // (clang-format Cpp11BracedListStyle).
    bool brace_init_continuation = false;
    // false: a wrapped function declarator name stays at the declaration's
    // indent instead of getting continuation indent (clang-format
    // IndentWrappedFunctionNames — LLVM breaks after the return type and
    // puts the name at column zero).
    bool indent_wrapped_function_names = true;

    // T3: wrapped operands of a binary/ternary expression align with the
    // first operand instead of plain continuation (clang-format
    // AlignOperands: Align / DontAlign).
    bool align_operands = true;
    // Wrapped ternary expressions put ?/: at line starts; controls where the
    // ?-column alignment anchor is recorded (clang-format
    // BreakBeforeTernaryOperators).
    bool break_before_ternary = true;

    // None: namespace bodies flush with the namespace keyword; Inner: only
    // bodies nested inside another namespace indent; All: every body indents
    // (clang-format NamespaceIndentation).
    enum class NamespaceIndentation { None, Inner, All };
    NamespaceIndentation namespace_indentation = NamespaceIndentation::None;
    bool indent_type_body = true;
    bool indent_case_label = false;
    bool indent_case_body = true;

    // Column offset of "public:" etc. relative to the type declaration line.
    // 0 = aligned with "class" (CLion default).
    int access_specifier_offset = 0;

    enum class ConstructorInitializerStyle {
        NormalIndent,
        ContinuationIndent,
        AlignFirstInitializer,
        AlignAfterColon,
    };

    ConstructorInitializerStyle constructor_initializers =
        ConstructorInitializerStyle::AlignFirstInitializer;

    bool operator==(const CppIndentStyle&) const = default;
};

} // namespace cind
