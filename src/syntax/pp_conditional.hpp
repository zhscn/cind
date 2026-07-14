#pragma once

#include "cpp_lexer/token.hpp"

#include <string_view>

namespace cind {

// Preprocessor conditional category, keyed on the directive keyword that
// follows '#'. Shared by the parser (brace reconciliation across #if/#else
// branches) and the indentation service (IndentPPDirectives depth), so the
// #if-family classification has a single source of truth.
enum class PPCat : std::uint8_t {
    Open,  // #if / #ifdef / #ifndef — opens a conditional level
    Alt,   // #else / #elif / #elifdef / #elifndef — an alternative branch
    Close, // #endif — closes a conditional level
    Other, // #define / #include / #pragma / … — brace-neutral, no nesting
};

// `kw_kind` is the kind of the first significant token after '#'; `kw_text`
// is its spelled text and is consulted only when kw_kind == Identifier
// (`if`/`else` lex as keywords even after '#', the rest stay identifiers).
inline PPCat pp_classify(TokenKind kw_kind, std::string_view kw_text) {
    switch (kw_kind) {
    case TokenKind::IfKw:
        return PPCat::Open;
    case TokenKind::ElseKw:
        return PPCat::Alt;
    case TokenKind::Identifier:
        if (kw_text == "ifdef" || kw_text == "ifndef") {
            return PPCat::Open;
        }
        if (kw_text == "endif") {
            return PPCat::Close;
        }
        if (kw_text == "elif" || kw_text == "elifdef" || kw_text == "elifndef") {
            return PPCat::Alt;
        }
        return PPCat::Other;
    default:
        return PPCat::Other;
    }
}

} // namespace cind
