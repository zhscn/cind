#pragma once

#include "document/document.hpp"
#include "indentation/indentation_service.hpp"
#include "syntax/analysis.hpp"

#include <string>

namespace cind {

struct EnterResult {
    std::string handler; // which Enter handler fired
    IndentDecision decision;
    TextOffset caret; // final caret position in the new revision
    DocumentChange change;
};

// Commands take the caller's Analyzer: trees come from its revision cache,
// speculative states are derived incrementally, and the cache is advanced
// across the commit — one keystroke costs one incremental relex + reparse
// instead of repeated full passes. The Analyzer-less overloads are
// conveniences for one-shot callers.

// The Enter handler pipeline (design.md §11): EnterBetweenBraces, then the
// newline-and-indent fallback. One transaction, one undo unit; the handler
// predicate is structural, never keystroke history.
EnterResult press_enter(Document& document, TextOffset caret, const CppIndentStyle& style,
                        Analyzer& analyzer);
EnterResult press_enter(Document& document, TextOffset caret, const CppIndentStyle& style);

// Explicit reindent of one line. Only the leading whitespace changes; lines
// starting inside raw strings or block comments are never touched.
IndentDecision indent_line(Document& document, std::uint32_t line, const CppIndentStyle& style,
                           Analyzer& analyzer);
IndentDecision indent_line(Document& document, std::uint32_t line, const CppIndentStyle& style);

struct TypeCharResult {
    bool reindented = false;
    IndentDecision decision; // populated when the reindent predicate fired
    TextOffset caret;        // final caret position in the new revision
    DocumentChange change;
};

// On-typing reindent (design.md §10.3): insert `ch` at the caret and, when a
// structural predicate holds, reindent the line in the same transaction —
// ':' completing a case label / access specifier / ctor initializer intro,
// '}' or '#' typed as the line's first content. One transaction, one undo
// unit; if the predicate fails the character is inserted unchanged.
TypeCharResult type_char(Document& document, TextOffset caret, char ch,
                         const CppIndentStyle& style, Analyzer& analyzer);
TypeCharResult type_char(Document& document, TextOffset caret, char ch,
                         const CppIndentStyle& style);

} // namespace cind
