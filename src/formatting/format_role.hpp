#pragma once

#include <cstdint>
#include <string_view>

namespace cind {

// What kind of slot the target line occupies; reported in every
// IndentDecision so results stay explainable.
enum class FormatRole : std::uint8_t {
    File,

    NamespaceBody,
    TypeBody,
    AccessSpecifierLabel,

    FunctionBody,
    CompoundBody,
    LambdaBody, // reported for compound bodies nested in expressions

    SingleStatementBody,
    ControlHeaderContinuation,

    CaseLabel,
    CaseBody,

    ConstructorInitializerIntro,
    ConstructorInitializerItem,

    ParenContinuation,
    BracketContinuation,
    TemplateArgsContinuation,
    BraceInit,
    StatementContinuation,

    PreprocessorDirective,
    ClosingToken,
    PreservedRawString,
    PreservedBlockComment,
    Opaque,
};

std::string_view format_role_name(FormatRole role);

} // namespace cind
