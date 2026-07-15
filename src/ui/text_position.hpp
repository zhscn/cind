#pragma once

#include "document/text.hpp"

#include <cstdint>

namespace cind::ui {

struct DisplayPosition {
    std::uint32_t line = 0;
    int column = 0;
};

TextOffset previous_grapheme(const Text& text, TextOffset offset);
TextOffset next_grapheme(const Text& text, TextOffset offset);

// Converts between UTF-8 byte offsets and the monospace cell grid shared by
// the terminal and pixel frontends. Tabs expand against `tab_width`; wide
// grapheme clusters use the same width rules as line composition.
int display_column(const Text& text, TextOffset offset, int tab_width);
TextOffset offset_at_display_column(const Text& text, DisplayPosition position, int tab_width);

int gutter_digits(std::uint32_t line_count);
int text_area_column(std::uint32_t line_count);

} // namespace cind::ui
