#pragma once

#include <cstdint>
#include <string_view>

namespace cind {

// Semantic cursor shape shared by input policy and frontend-independent
// presentation. Presenters choose native geometry or terminal control
// sequences for the same value.
enum class CursorShape : std::uint8_t {
    Beam,
    Block,
    Underline,
};

constexpr std::string_view cursor_shape_name(CursorShape shape) {
    switch (shape) {
    case CursorShape::Beam:
        return "beam";
    case CursorShape::Block:
        return "block";
    case CursorShape::Underline:
        return "underline";
    }
    return "unknown";
}

} // namespace cind
