#include "document/line_index.hpp"

#include <algorithm>
#include <stdexcept>

namespace cind {

LineIndex::LineIndex(std::string_view text) : text_size_(static_cast<std::uint32_t>(text.size())) {
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            line_starts_.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
}

void LineIndex::check_line(std::uint32_t line) const {
    if (line >= line_count()) {
        throw std::out_of_range("LineIndex: line out of range");
    }
}

TextOffset LineIndex::line_start(std::uint32_t line) const {
    check_line(line);
    return TextOffset{line_starts_[line]};
}

TextOffset LineIndex::line_content_end(std::uint32_t line) const {
    check_line(line);
    if (line + 1 < line_count()) {
        return TextOffset{line_starts_[line + 1] - 1}; // before the '\n'
    }
    return TextOffset{text_size_};
}

TextRange LineIndex::line_range(std::uint32_t line) const {
    check_line(line);
    std::uint32_t end = line + 1 < line_count() ? line_starts_[line + 1] : text_size_;
    return make_range(line_starts_[line], end);
}

TextRange LineIndex::line_content_range(std::uint32_t line) const {
    return TextRange{line_start(line), line_content_end(line)};
}

LinePosition LineIndex::position(TextOffset offset) const {
    if (offset.value > text_size_) {
        throw std::out_of_range("LineIndex: offset out of range");
    }
    auto it = std::ranges::upper_bound(line_starts_, offset.value);
    auto line = static_cast<std::uint32_t>(it - line_starts_.begin() - 1);
    return LinePosition{line, offset.value - line_starts_[line]};
}

TextOffset LineIndex::offset(LinePosition position) const {
    check_line(position.line);
    TextRange range = line_range(position.line);
    if (position.byte_column > range.length()) {
        throw std::out_of_range("LineIndex: column out of range");
    }
    return TextOffset{range.start.value + position.byte_column};
}

} // namespace cind
