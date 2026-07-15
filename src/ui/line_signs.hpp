#pragma once

#include "document/text.hpp"

#include <cstdint>

namespace cind::ui {

// Unsaved-change sign for one line (the classic gutter marks; git HEAD can
// replace "saved file" as the baseline later without touching the model).
enum class SignKind : std::uint8_t {
    None,
    Added,
    Modified,
    DeletedAbove, // lines were deleted just above this line
};

// Line-level unsaved-change signs: which lines of `current` differ from
// `baseline` (the file as last saved; a git HEAD blob works the same way).
// Built on the structural diff (diff_spans), so shared rope chunks keep a
// recompute at O(changed bytes + log n) — cheap enough per keystroke even at
// half a million lines.
//
// The single changed window maps to one contiguous stretch of signs, so the
// result is a compact span with O(1) per-line lookup, never a materialized
// per-line map (an edit at both ends of a large file would otherwise mark —
// and allocate — every line in between).
struct LineSigns {
    std::uint32_t first = 0;    // first signed line (current coordinates)
    std::uint32_t modified = 0; // Modified on [first, first + modified)
    std::uint32_t added = 0;    // Added on the next `added` lines
    bool deleted = false;       // DeletedAbove on `boundary`
    std::uint32_t boundary = 0;

    SignKind at(std::uint32_t line) const {
        if (line >= first) {
            if (line < first + modified) {
                return SignKind::Modified;
            }
            if (line < first + modified + added) {
                return SignKind::Added;
            }
        }
        // Modified/Added win when the clamped boundary lands on them.
        if (deleted && line == boundary) {
            return SignKind::DeletedAbove;
        }
        return SignKind::None;
    }
    bool empty() const { return modified == 0 && added == 0 && !deleted; }
};

LineSigns line_signs(const Text& baseline, const Text& current);

} // namespace cind::ui
