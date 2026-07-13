#pragma once

#include "document/text_types.hpp"

#include <string_view>
#include <vector>

namespace cind {

// Maps between byte offsets and line/column positions. Lines split on '\n'
// only; a trailing '\n' yields a final empty line. Always has >= 1 line.
class LineIndex {
public:
    LineIndex() : line_starts_{0} {}
    explicit LineIndex(std::string_view text);

    std::uint32_t line_count() const { return static_cast<std::uint32_t>(line_starts_.size()); }
    std::uint32_t text_size() const { return text_size_; }

    TextOffset line_start(std::uint32_t line) const;
    // Offset just past the last content byte, before the terminating '\n'.
    TextOffset line_content_end(std::uint32_t line) const;
    // [line_start, next line_start): includes the terminating '\n' if present.
    TextRange line_range(std::uint32_t line) const;
    TextRange line_content_range(std::uint32_t line) const;

    // offset must be <= text_size(); text_size() maps to the last line.
    LinePosition position(TextOffset offset) const;
    // byte_column must not point past the line's terminating '\n'.
    TextOffset offset(LinePosition position) const;

private:
    void check_line(std::uint32_t line) const;

    std::vector<std::uint32_t> line_starts_;
    std::uint32_t text_size_ = 0;
};

} // namespace cind
