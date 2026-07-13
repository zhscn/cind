#pragma once

#include <cstdint>
#include <string_view>

namespace cind {

enum class SyntaxKind : std::uint8_t {
    TranslationUnit,

    PreprocessorDirective,

    NamespaceDecl,
    NamespaceBody,

    ClassDecl, // class/struct/union/enum head, with or without body
    ClassBody,
    AccessSpecifierLabel,

    OpaqueDeclaration, // declaration/statement fallback; expressions stay opaque
    FunctionDefinition,
    CtorInitializerList,
    CtorInitializer,

    ParenGroup,
    BracketGroup,
    BraceGroup, // braced initializer; not a statement body
    TemplateArgumentList,

    CompoundStatement,
    IfStatement,
    ElseClause,
    ForStatement,
    WhileStatement,
    DoStatement,
    SwitchStatement,
    CaseSection, // label + its statements
    CaseLabel,

    MissingToken, // zero-length: an expected token that is not there
    Error,
};

std::string_view syntax_kind_name(SyntaxKind kind);

} // namespace cind
