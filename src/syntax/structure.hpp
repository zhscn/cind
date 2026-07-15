#pragma once

#include "document/text.hpp"
#include "syntax/syntax_tree.hpp"

#include <optional>

namespace cind {

// Balanced-unit ("sexp") primitives over the CST, for puni-style structural
// editing. A unit is a bracket pair (with everything inside), a template
// argument list, or a single non-trivia token; comments count as trivia for
// navigation but as units for soft kill. All functions are pure queries.

// The next/previous whole unit strictly after/before `from`, skipping
// trivia. nullopt when blocked by the enclosing group's closer (or ends).
std::optional<TextRange> sexp_forward(const SyntaxTree& tree, TextOffset from);
std::optional<TextRange> sexp_backward(const SyntaxTree& tree, TextOffset from);

// Full range (delimiters included) of the innermost bracket-delimited
// construct containing `offset`; nullopt at the top level.
std::optional<TextRange> enclosing_list(const SyntaxTree& tree, TextOffset offset);

// Full range of the properly nested bracket pair whose opening or closing
// delimiter begins at `offset`. Returns nullopt for non-delimiters, unmatched
// delimiters and crossing pairs.
std::optional<TextRange> matching_bracket_range(const SyntaxTree& tree, TextOffset offset);

// Next bigger syntactic range: token -> group interior -> whole group ->
// parent node ... Used for expand-region. nullopt once the whole tree is
// selected. An empty `range` seeds from the token/node at its position.
std::optional<TextRange> expand_selection(const SyntaxTree& tree, TextRange range);

// Soft kill to end of line: whole units only, so a unit straddling the line
// end is swallowed entirely and the enclosing closer is never crossed. At
// the very end of a line the newline itself is killed. Empty range = no-op.
TextRange soft_kill_end(const SyntaxTree& tree, const Text& text, TextOffset from);

} // namespace cind
