#pragma once

#include "editor/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace cind {

enum class PointerTargetKind : std::uint8_t {
    DocumentText,
    DocumentGutter,
    PopupHeader,
    PopupItem,
    Status,
    Echo,
    Region,
};

struct PointerEvent {
    PointerTargetKind target = PointerTargetKind::Region;
    std::optional<WindowId> window;
    std::optional<std::uint32_t> document_line;
    std::optional<std::uint32_t> display_column;
    std::optional<std::size_t> popup_item;
};

} // namespace cind
