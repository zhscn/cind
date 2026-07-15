#include "ui/text_position.hpp"

#include "ui/char_width.hpp"

#include <algorithm>
#include <vector>

namespace cind::ui {

namespace {

std::vector<std::uint32_t> grapheme_boundaries(std::string_view text) {
    std::vector<std::uint32_t> boundaries{0};
    std::uint32_t at = 0;
    while (at < text.size()) {
        const GraphemeDecode decoded = decode_grapheme(text.substr(at));
        at += static_cast<std::uint32_t>(decoded.bytes);
        boundaries.push_back(at);
    }
    return boundaries;
}

} // namespace

TextOffset previous_grapheme(const Text& text, TextOffset offset) {
    if (offset.value == 0) {
        return offset;
    }
    const LinePosition position = text.position(offset);
    const TextOffset line_start = text.line_start(position.line);
    if (offset == line_start) {
        return TextOffset{offset.value - 1};
    }
    const std::string prefix = text.substring(TextRange{line_start, offset});
    const std::vector<std::uint32_t> boundaries = grapheme_boundaries(prefix);
    return TextOffset{line_start.value + boundaries[boundaries.size() - 2]};
}

TextOffset next_grapheme(const Text& text, TextOffset offset) {
    if (offset.value >= text.size_bytes()) {
        return offset;
    }
    const LinePosition position = text.position(offset);
    const TextRange content = text.line_content_range(position.line);
    if (offset >= content.end) {
        return TextOffset{offset.value + 1};
    }
    const std::string line = text.substring(content);
    const std::vector<std::uint32_t> boundaries = grapheme_boundaries(line);
    const std::uint32_t relative = offset.value - content.start.value;
    const auto next = std::ranges::upper_bound(boundaries, relative);
    return TextOffset{content.start.value + *next};
}

int display_column(const Text& text, TextOffset offset, int tab_width) {
    const std::uint32_t start = text.line_start(text.position(offset).line).value;
    const std::string bytes = text.substring(make_range(start, offset.value));
    std::string_view rest = bytes;
    int column = 0;
    while (!rest.empty()) {
        if (rest.front() == '\t') {
            column += tab_width - column % tab_width;
            rest.remove_prefix(1);
            continue;
        }
        const GraphemeDecode decoded = decode_grapheme(rest);
        column += decoded.width;
        rest.remove_prefix(static_cast<std::size_t>(decoded.bytes));
    }
    return column;
}

TextOffset offset_at_display_column(const Text& text, DisplayPosition position, int tab_width) {
    const TextRange content = text.line_content_range(position.line);
    const std::string bytes = text.substring(content);
    std::string_view rest = bytes;
    std::uint32_t offset = content.start.value;
    int current = 0;
    while (!rest.empty() && current < position.column) {
        if (rest.front() == '\t') {
            current += tab_width - current % tab_width;
            ++offset;
            rest.remove_prefix(1);
            continue;
        }
        const GraphemeDecode decoded = decode_grapheme(rest);
        current += decoded.width;
        offset += static_cast<std::uint32_t>(decoded.bytes);
        rest.remove_prefix(static_cast<std::size_t>(decoded.bytes));
    }
    return TextOffset{offset};
}

int gutter_digits(std::uint32_t line_count) {
    int digits = 1;
    for (std::uint32_t value = line_count; value >= 10; value /= 10) {
        ++digits;
    }
    return digits;
}

int text_area_column(std::uint32_t line_count) {
    return gutter_digits(line_count) + 2;
}

} // namespace cind::ui
