#pragma once

#include <cstdint>
#include <string_view>

namespace cind::ui {

struct Utf8Decode {
    char32_t cp = 0;
    int bytes = 1; // bytes consumed; invalid sequences consume 1 and yield U+FFFD
};

struct GraphemeDecode {
    int bytes = 1;
    int width = 1;
};

// Decodes the first code point of a non-empty UTF-8 view. Tolerant: invalid
// lead/continuation bytes decode as U+FFFD one byte at a time, so a scan
// never stalls or overruns.
Utf8Decode decode_utf8(std::string_view s);

// Decodes the first Unicode extended grapheme cluster and reports its terminal
// cell width. Invalid UTF-8 consumes one byte as U+FFFD.
GraphemeDecode decode_grapheme(std::string_view s);

// Unicode character width from utf8proc. Control characters are the caller's
// business (tabs are expanded before measuring).
int code_point_width(char32_t cp);

// Sum of extended-grapheme widths of a UTF-8 string (no tabs expected).
int display_width(std::string_view s);

// Longest prefix that fits in `width` cells without splitting a UTF-8 sequence
// or extended grapheme cluster.
std::string_view clip_to_display_width(std::string_view s, int width);

} // namespace cind::ui
