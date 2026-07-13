#pragma once

namespace cind {

struct CppIndentStyle {
    int indent_width = 4;
    int continuation_indent = 4;
    int tab_width = 4;
    bool use_tabs = false;

    bool indent_namespace_body = false;
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
};

} // namespace cind
