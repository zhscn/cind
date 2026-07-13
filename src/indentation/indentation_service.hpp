#pragma once

#include "document/snapshot.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "formatting/format_role.hpp"
#include "syntax/syntax_tree.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cind {

struct IndentDecision {
    int target_column = 0;
    std::string indentation_text;
    FormatRole role = FormatRole::File;
    std::optional<TextOffset> anchor;
    // Raw-string / block-comment protection: the line must not be touched.
    bool preserve = false;
    std::vector<std::string> trace;
};

// Pure query (never edits): the indentation `line` should have, given the
// structure of `tree` (which must be a parse of `snapshot`'s text).
IndentDecision compute_line_indent(const DocumentSnapshot& snapshot, const SyntaxTree& tree,
                                   std::uint32_t line, const CppIndentStyle& style);

} // namespace cind
