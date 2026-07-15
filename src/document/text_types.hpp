#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace cind {

// UTF-8 byte offset into a document at one revision.
struct TextOffset {
    std::uint32_t value = 0;

    friend constexpr auto operator<=>(TextOffset, TextOffset) = default;
};

// Half-open byte range [start, end).
struct TextRange {
    TextOffset start;
    TextOffset end;

    constexpr std::uint32_t length() const { return end.value - start.value; }
    constexpr bool empty() const { return start == end; }
    constexpr bool contains(TextOffset offset) const { return start <= offset && offset < end; }

    friend constexpr bool operator==(TextRange, TextRange) = default;
};

constexpr TextRange make_range(std::uint32_t start, std::uint32_t end) {
    return TextRange{TextOffset{start}, TextOffset{end}};
}

using RevisionId = std::uint64_t;
using DocumentId = std::uint32_t;

struct VersionedRange {
    RevisionId revision = 0;
    TextRange range;
};

struct LinePosition {
    std::uint32_t line = 0;
    std::uint32_t byte_column = 0;

    friend constexpr auto operator<=>(LinePosition, LinePosition) = default;
};

struct TextEdit {
    TextRange old_range;
    std::string new_text;
};

// The changed windows of a structural diff, one range per side (see
// diff_spans in text.hpp). Both ranges share their start offset; either may
// be empty (pure insertion / pure deletion).
struct DiffSpans {
    TextRange a_range;
    TextRange b_range;
};

struct DocumentChange {
    RevisionId old_revision = 0;
    RevisionId new_revision = 0;
    // Normalized: old-revision coordinates, ascending by start, non-overlapping.
    std::vector<TextEdit> edits;
    TextRange affected_old_range;
    TextRange affected_new_range;
};

} // namespace cind
