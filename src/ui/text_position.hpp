#pragma once

#include "document/text.hpp"

#include <cstdint>

namespace cind::ui {

TextOffset previous_code_point(const Text& text, TextOffset offset);
TextOffset next_code_point(const Text& text, TextOffset offset);

// Converts between UTF-8 byte offsets and the monospace cell grid shared by
// the terminal and pixel frontends. Tabs expand against `tab_width`; wide
// code points use the same width table as line composition.
int display_column(const Text& text, TextOffset offset, int tab_width);
TextOffset offset_at_display_column(const Text& text, std::uint32_t line, int column,
                                    int tab_width);

int gutter_digits(std::uint32_t line_count);
int text_area_column(std::uint32_t line_count);

} // namespace cind::ui
