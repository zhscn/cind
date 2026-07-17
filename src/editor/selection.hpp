#pragma once

#include "document/text_types.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

enum class SelectionGranularity : std::uint8_t {
    Character,
    Line,
    Block,
    Node,
};

constexpr std::string_view selection_granularity_name(SelectionGranularity granularity) {
    switch (granularity) {
    case SelectionGranularity::Character:
        return "char";
    case SelectionGranularity::Line:
        return "line";
    case SelectionGranularity::Block:
        return "block";
    case SelectionGranularity::Node:
        return "node";
    }
    return "unknown";
}

struct SelectionRange {
    TextOffset anchor;
    TextOffset head;
    SelectionGranularity granularity = SelectionGranularity::Character;

    TextRange ordered() const {
        return anchor < head ? TextRange{anchor, head} : TextRange{head, anchor};
    }

    friend constexpr auto operator<=>(const SelectionRange&, const SelectionRange&) = default;
};

// A View selection is directional and always contains a primary range. Metadata
// is an opaque Scheme external representation: the editor core retains it but
// leaves its interpretation to the active input strategy.
struct ViewSelection {
    std::vector<SelectionRange> ranges;
    std::size_t primary = 0;
    std::string metadata = "()";

    friend bool operator==(const ViewSelection&, const ViewSelection&) = default;
};

// Compact name for APIs that accept exactly one directional range.
using SelectionEndpoints = SelectionRange;

} // namespace cind
