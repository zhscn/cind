#pragma once

#include "cpp_lexer/lexer_state.hpp"
#include "cpp_lexer/token.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace cind {

struct LexOutput {
    // Contiguous, non-overlapping, covers the whole input; terminated by a
    // zero-length EndOfFile token.
    std::vector<Token> tokens;
    // State entering each line; line_states[0] is the initial (default) state.
    // Size equals the line count of the input.
    std::vector<LexerState> line_states;
};

class Text;

// Full-file lossless lex. Never fails; unrecognized bytes become Invalid
// tokens and unterminated constructs are flagged, not dropped. The Text
// overload scans chunk by chunk without materializing the string.
LexOutput lex(std::string_view text);
LexOutput lex(const Text& text);

struct TextEdit;

// Incremental relex (design.md §7): `old_tokens`/`old_line_states` are the
// lex of `old_text` (borrowed, not consumed), `edits` the normalized edit
// list (old coordinates, ascending, non-overlapping) that turns `old_text`
// into `new_text`. Rescans only the damaged window: restarts at the nearest
// line start at or before the damage whose entry state is default and that
// begins a token, and stops once past the damage at a line start where old
// and new agree again ("新旧 token 与出向 state 相同即停止传播"). The
// untouched prefix and suffix are copied over, the suffix offset-shifted.
//
// Result is byte-for-byte equal to lex(new_text) — fuzz-locked.
LexOutput relex(const std::vector<Token>& old_tokens,
                const std::vector<LexerState>& old_line_states, const Text& old_text,
                const Text& new_text, std::span<const TextEdit> edits);

} // namespace cind
