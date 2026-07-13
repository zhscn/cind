#pragma once

#include <cstdint>

namespace cind {

// How a zero-length anchor behaves when text is inserted exactly at it.
enum class AnchorAffinity {
    BeforeInsertion, // anchor stays before the inserted text
    AfterInsertion,  // anchor moves after the inserted text
};

using AnchorId = std::uint32_t;

} // namespace cind
