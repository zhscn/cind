#pragma once

#include "document/text_types.hpp"

#include <string>

namespace cind {

// A frontend-independent label projected at a document byte position. The
// provider owns the meaning of the label; presenters only replace the cell at
// the resolved display position without changing document layout.
struct PositionHint {
    TextOffset position;
    std::string label;

    friend bool operator==(const PositionHint&, const PositionHint&) = default;
};

} // namespace cind
