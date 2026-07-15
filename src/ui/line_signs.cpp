#include "ui/line_signs.hpp"

#include <algorithm>

namespace cind::ui {

namespace {

bool at_line_start(const Text& text, TextOffset offset) {
    return offset == text.line_start(text.position(offset).line);
}

// Lines covered by a changed window, widened to whole lines. For a window
// that starts and ends exactly on line boundaries the count is the number of
// whole lines in between (0 for an empty window — a pure boundary point);
// otherwise every line the window touches counts.
struct LineSpan {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

LineSpan touched_lines(const Text& text, TextRange range, bool clean_boundaries) {
    LineSpan span;
    span.first = text.position(range.start).line;
    if (clean_boundaries) {
        span.count = text.position(range.end).line - span.first;
        return span;
    }
    std::uint32_t last = text.position(range.end).line;
    // A non-empty window ending exactly at a line start does not touch the
    // line that begins there.
    if (range.end > range.start && at_line_start(text, range.end)) {
        --last;
    }
    span.count = last - span.first + 1;
    return span;
}

} // namespace

LineSigns line_signs(const Text& baseline, const Text& current) {
    LineSigns signs;
    const std::optional<DiffSpans> spans = diff_spans(baseline, current);
    if (!spans) {
        return signs;
    }

    // Text always has one logical line, even when it contains no bytes. That
    // placeholder is not an existing line for change-sign purposes: content
    // entered into an empty file consists entirely of added lines.
    if (baseline.empty()) {
        signs.added = current.line_count();
        if (at_line_start(current, current.end_offset())) {
            --signs.added; // the logical empty line after a trailing newline
        }
        return signs;
    }

    // Whole-line block replace (the common case for Enter, paste, and line
    // deletion) is detected exactly; anything else widens to full lines.
    const bool clean = at_line_start(current, spans->b_range.start) &&
                       at_line_start(baseline, spans->a_range.end) &&
                       at_line_start(current, spans->b_range.end);
    const LineSpan old_span = touched_lines(baseline, spans->a_range, clean);
    const LineSpan new_span = touched_lines(current, spans->b_range, clean);

    signs.first = new_span.first;
    signs.modified = std::min(old_span.count, new_span.count);
    signs.added = new_span.count - signs.modified;
    if (old_span.count > new_span.count) {
        signs.deleted = true;
        const std::uint32_t last_line = current.line_count() - 1;
        signs.boundary = std::min(new_span.first + new_span.count, last_line);
    }
    return signs;
}

} // namespace cind::ui
