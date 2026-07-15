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

    // Phantom scope opened at #else/#elif when the first branch closed scopes
    // the conditional did not open (`do { #if } while(A); #else } while(B);`):
    // the alternative branch's '}' closes it instead of an outer container.
    PPReopenedScope,

    MissingToken, // zero-length: an expected token that is not there
    Error,
};

std::string_view syntax_kind_name(SyntaxKind kind);

} // namespace cind
