#pragma once

#include "document/document.hpp"
#include "indentation/indentation_service.hpp"

#include <string>

namespace cind {

struct EnterResult {
    std::string handler; // which Enter handler fired
    IndentDecision decision;
    TextOffset caret; // final caret position in the new revision
    DocumentChange change;
};

// The Enter handler pipeline (design.md §11): EnterBetweenBraces, then the
// newline-and-indent fallback. One transaction, one undo unit; the handler
// predicate is structural, never keystroke history.
EnterResult press_enter(Document& document, TextOffset caret, const CppIndentStyle& style);

// Explicit reindent of one line. Only the leading whitespace changes; lines
// starting inside raw strings or block comments are never touched.
IndentDecision indent_line(Document& document, std::uint32_t line, const CppIndentStyle& style);

} // namespace cind
