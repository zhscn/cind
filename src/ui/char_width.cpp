#include "ui/char_width.hpp"

namespace cind::ui {

Utf8Decode decode_utf8(std::string_view s) {
    const auto b0 = static_cast<unsigned char>(s[0]);
    if (b0 < 0x80) {
        return {b0, 1};
    }
    int len = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07u;
    } else {
        return {0xFFFD, 1}; // stray continuation or invalid lead byte
    }
    if (s.size() < static_cast<std::size_t>(len)) {
        return {0xFFFD, 1};
    }
    for (int i = 1; i < len; ++i) {
        const auto b = static_cast<unsigned char>(s[static_cast<std::size_t>(i)]);
        if ((b & 0xC0) != 0x80) {
            return {0xFFFD, 1};
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    return {cp, len};
}

int code_point_width(char32_t cp) {
    // Zero width: combining marks and joiners (coarse ranges).
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200B ||
        cp == 0x200C || cp == 0x200D) {
        return 0;
    }
    // Wide: East Asian Wide/Fullwidth blocks plus common emoji planes,
    // mirroring wcwidth's behavior closely enough for a text editor.
    if ((cp >= 0x1100 && cp <= 0x115F) || // Hangul Jamo
        (cp >= 0x2E80 && cp <= 0x303E) || // CJK radicals .. punctuation
        (cp >= 0x3041 && cp <= 0x33FF) || // kana .. CJK compatibility
        (cp >= 0x3400 && cp <= 0x4DBF) || // CJK ext A
        (cp >= 0x4E00 && cp <= 0x9FFF) || // CJK unified
        (cp >= 0xA000 && cp <= 0xA4CF) || // Yi
        (cp >= 0xAC00 && cp <= 0xD7A3) || // Hangul syllables
        (cp >= 0xF900 && cp <= 0xFAFF) || // CJK compatibility ideographs
        (cp >= 0xFE30 && cp <= 0xFE4F) || // CJK compatibility forms
        (cp >= 0xFF00 && cp <= 0xFF60) || // fullwidth forms
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F9FF) || // emoji & pictographs
        (cp >= 0x20000 && cp <= 0x3FFFD)) {                                   // CJK ext B..
        return 2;
    }
    return 1;
}

int display_width(std::string_view s) {
    int width = 0;
    while (!s.empty()) {
        const Utf8Decode d = decode_utf8(s);
        width += code_point_width(d.cp);
        s.remove_prefix(static_cast<std::size_t>(d.bytes));
    }
    return width;
}

} // namespace cind::ui
