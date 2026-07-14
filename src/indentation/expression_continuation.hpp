#pragma once

#include "document/snapshot.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "syntax/syntax_tree.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cind {

// T3 expression-continuation engine (design.md §10.2, 决策十修订).
//
// Replays clang-format's ContinuationIndenter column rules over the actual
// layout of the statement containing `line` and returns the column the
// line's first token should get. Unlike clang-format we never choose break
// points — they are facts of the file — so this is a single linear pass with
// no penalties or optimizer.
//
// `controlling` is the node the generic path chose for the line; the engine
// widens it to the enclosing statement region itself. Returns nullopt when
// the region is not suitable (preprocessor body, oversized region, query
// token not found) — the caller falls back to the T2 contribution table.
std::optional<int> expression_continuation_column(const DocumentSnapshot& snapshot,
                                                  const SyntaxTree& tree, std::uint32_t line,
                                                  SyntaxNodeId controlling,
                                                  const CppIndentStyle& style,
                                                  std::vector<std::string>& trace);

} // namespace cind
