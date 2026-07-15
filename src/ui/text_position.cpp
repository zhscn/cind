#include "ui/text_position.hpp"

#include "ui/char_width.hpp"

namespace cind::ui {

namespace {

bool is_continuation_byte(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

} // namespace

TextOffset previous_code_point(const Text& text, TextOffset offset) {
    if (offset.value == 0) {
        return offset;
    }
    std::uint32_t at = offset.value - 1;
    while (at > 0 && is_continuation_byte(text.byte_at(TextOffset{at}))) {
        --at;
    }
    return TextOffset{at};
}

TextOffset next_code_point(const Text& text, TextOffset offset) {
    if (offset.value >= text.size_bytes()) {
        return offset;
    }
    std::uint32_t at = offset.value + 1;
    while (at < text.size_bytes() && is_continuation_byte(text.byte_at(TextOffset{at}))) {
        ++at;
    }
    return TextOffset{at};
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
        const Utf8Decode decoded = decode_utf8(rest);
        column += code_point_width(decoded.cp);
        rest.remove_prefix(static_cast<std::size_t>(decoded.bytes));
    }
    return column;
}

TextOffset offset_at_display_column(const Text& text, std::uint32_t line, int column,
                                    int tab_width) {
    const TextRange content = text.line_content_range(line);
    const std::string bytes = text.substring(content);
    std::string_view rest = bytes;
    std::uint32_t offset = content.start.value;
    int current = 0;
    while (!rest.empty() && current < column) {
        if (rest.front() == '\t') {
            current += tab_width - current % tab_width;
            ++offset;
            rest.remove_prefix(1);
            continue;
        }
        const Utf8Decode decoded = decode_utf8(rest);
        current += code_point_width(decoded.cp);
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
