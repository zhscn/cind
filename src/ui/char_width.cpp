#include "ui/char_width.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <limits>

namespace cind::ui {

Utf8Decode decode_utf8(std::string_view s) {
    utf8proc_int32_t code_point = 0;
    const auto available = static_cast<utf8proc_ssize_t>(
        std::min(s.size(), static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())));
    const utf8proc_ssize_t length = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(s.data()), available, &code_point);
    if (length <= 0) {
        return {0xFFFD, 1};
    }
    return {static_cast<char32_t>(code_point), static_cast<int>(length)};
}

int code_point_width(char32_t cp) {
    return std::max(0, utf8proc_charwidth(static_cast<utf8proc_int32_t>(cp)));
}

GraphemeDecode decode_grapheme(std::string_view s) {
    const Utf8Decode first = decode_utf8(s);
    int width = code_point_width(first.cp);
    bool emoji_presentation = first.cp >= 0x1F1E6 && first.cp <= 0x1F1FF;
    int bytes = first.bytes;
    utf8proc_int32_t state = 0;
    char32_t previous = first.cp;
    while (static_cast<std::size_t>(bytes) < s.size()) {
        const Utf8Decode current = decode_utf8(s.substr(static_cast<std::size_t>(bytes)));
        if (utf8proc_grapheme_break_stateful(static_cast<utf8proc_int32_t>(previous),
                                             static_cast<utf8proc_int32_t>(current.cp), &state)) {
            break;
        }
        width = std::max(width, code_point_width(current.cp));
        emoji_presentation = emoji_presentation || current.cp == 0xFE0F;
        bytes += current.bytes;
        previous = current.cp;
    }
    if (emoji_presentation) {
        width = std::max(width, 2);
    }
    return {bytes, width};
}

int display_width(std::string_view s) {
    int width = 0;
    while (!s.empty()) {
        const GraphemeDecode d = decode_grapheme(s);
        width += d.width;
        s.remove_prefix(static_cast<std::size_t>(d.bytes));
    }
    return width;
}

std::string_view clip_to_display_width(std::string_view s, int width) {
    if (width <= 0) {
        return s.substr(0, 0);
    }
    std::size_t bytes = 0;
    int used = 0;
    while (bytes < s.size()) {
        const GraphemeDecode decoded = decode_grapheme(s.substr(bytes));
        if (used + decoded.width > width) {
            break;
        }
        used += decoded.width;
        bytes += static_cast<std::size_t>(decoded.bytes);
    }
    return s.substr(0, bytes);
}

} // namespace cind::ui
