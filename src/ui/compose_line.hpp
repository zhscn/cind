#pragma once

#include "cpp_lexer/token_buffer.hpp"
#include "ui/scene.hpp"

#include <optional>
#include <string_view>

namespace cind::ui {

// Lays one document line out into styled runs: token highlighting, selection
// splitting, tab expansion, wide-glyph measurement, and horizontal clipping
// to [left_col, left_col + width). Pure layout — no escape codes, no
// terminal knowledge — so it is shared by every presenter and unit-testable
// without a pty.
struct LineComposeInput {
    std::string_view text;          // line content, without the newline
    std::uint32_t start_offset = 0; // absolute byte offset of text[0]
    int tab_width = 4;
    int left_col = 0; // first visible display column
    int width = 80;   // visible columns in the text area
    std::optional<TextRange> selection;
};

std::vector<Run> build_line_runs(const LineComposeInput& in, const TokenBuffer& tokens);

} // namespace cind::ui
