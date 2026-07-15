#include "ui/compose_line.hpp"

#include "ui/char_width.hpp"

#include <algorithm>

namespace cind::ui {

std::vector<Run> build_line_runs(const LineComposeInput& in, const TokenBuffer& tokens) {
    std::vector<Run> runs;
    Run current;
    bool open = false;

    const auto flush = [&] {
        if (open && !current.text.empty()) {
            runs.push_back(std::move(current));
        }
        current = Run{};
        open = false;
    };
    const auto emit = [&](std::string_view glyph, int col, StyleClass style, bool selected) {
        if (open && (style != current.style || selected != current.selected)) {
            flush();
        }
        if (!open) {
            current.col = col - in.left_col;
            current.style = style;
            current.selected = selected;
            open = true;
        }
        current.text.append(glyph);
    };

    // First token overlapping the line, then walked forward in lockstep.
    auto it = std::ranges::lower_bound(
        tokens, TextOffset{in.start_offset}, [](TextOffset a, TextOffset b) { return a < b; },
        [](const Token& t) { return t.range.end; });

    const int right = in.left_col + in.width;
    int col = 0;
    std::size_t at = 0;
    while (at < in.text.size() && col < right) {
        const std::uint32_t offset = in.start_offset + static_cast<std::uint32_t>(at);
        while (it != tokens.end() && it->range.end.value <= offset) {
            ++it;
        }
        StyleClass style = StyleClass::Text;
        if (it != tokens.end() && it->range.start.value <= offset) {
            style = style_of(*it);
        }
        const bool selected = in.selection && in.selection->contains(TextOffset{offset});

        const char c = in.text[at];
        if (c == '\t') {
            const int cell = in.tab_width - col % in.tab_width;
            for (int k = 0; k < cell; ++k) {
                if (col + k >= right) {
                    break;
                }
                if (col + k >= in.left_col) {
                    emit(" ", col + k, style, selected);
                }
            }
            col += cell;
            ++at;
            continue;
        }

        const Utf8Decode d = decode_utf8(in.text.substr(at));
        const int cell = code_point_width(d.cp);
        if (cell == 0) {
            // Combining mark: attach to the open run when visible.
            if (open) {
                current.text.append(in.text.substr(at, static_cast<std::size_t>(d.bytes)));
            }
            at += static_cast<std::size_t>(d.bytes);
            continue;
        }
        if (col + cell > right) {
            break; // a wide glyph that would cross the right edge is dropped
        }
        if (col >= in.left_col) {
            emit(in.text.substr(at, static_cast<std::size_t>(d.bytes)), col, style, selected);
        } else if (col + cell > in.left_col) {
            // Wide glyph straddling the left clip edge: pad its visible half.
            emit(" ", in.left_col, style, selected);
        }
        col += cell;
        at += static_cast<std::size_t>(d.bytes);
    }
    flush();
    return runs;
}

} // namespace cind::ui
