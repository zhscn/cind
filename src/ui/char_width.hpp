#pragma once

#include <cstdint>
#include <string_view>

namespace cind::ui {

struct Utf8Decode {
    char32_t cp = 0;
    int bytes = 1; // bytes consumed; invalid sequences consume 1 and yield U+FFFD
};

// Decodes the first code point of a non-empty UTF-8 view. Tolerant: invalid
// lead/continuation bytes decode as U+FFFD one byte at a time, so a scan
// never stalls or overruns.
Utf8Decode decode_utf8(std::string_view s);

// Approximate wcwidth: 0 for combining marks and zero-width joiners, 2 for
// East Asian wide/fullwidth ranges and emoji, 1 otherwise. Control characters
// are the caller's business (tabs are expanded before measuring).
int code_point_width(char32_t cp);

// Sum of code-point widths of a UTF-8 string (no tabs expected).
int display_width(std::string_view s);

} // namespace cind::ui
