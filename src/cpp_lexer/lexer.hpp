#pragma once

#include "cpp_lexer/lexer_state.hpp"
#include "cpp_lexer/token.hpp"
#include "cpp_lexer/token_buffer.hpp"

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

// Long-lived token streams (SyntaxTree) hold a chunked TokenBuffer so the
// splice below is sublinear (design.md §214); the scanned window itself stays
// a flat LexOutput.

// The raw relex plan: which old tokens/lines survive and what was rescanned.
// Old tokens [keep_tokens, stop_token) are replaced by `scanned.tokens`; old
// line states [restart_line + 1, stop_line + 1) by `scanned.line_states`.
// Token byte offsets from stop_token on shift by `delta`. When the scan ran
// to EOF, stop_token/stop_line are the old vector sizes and delta is
// meaningless (nothing survives past the window).
struct RelexSplice {
    std::size_t keep_tokens = 0;
    std::size_t stop_token = 0;
    std::uint32_t restart_line = 0;
    std::uint32_t stop_line = 0;
    std::int64_t delta = 0;
    bool hit_eof = false;
    LexOutput scanned;
};

RelexSplice relex_scan(const TokenBuffer& old_tokens,
                       const std::vector<LexerState>& old_line_states, const Text& old_text,
                       const Text& new_text, std::span<const TextEdit> edits);

// Applies a relex_scan plan to the containers it was computed from: splices
// the rescanned window in and shifts the surviving suffix (O(chunks), not
// O(tokens) — the whole point of the chunked buffer).
void relex_apply(TokenBuffer& tokens, std::vector<LexerState>& line_states,
                 RelexSplice&& splice);

// In-place variant of relex() (scan + apply). Same equivalence guarantee.
RelexSplice relex_in_place(TokenBuffer& tokens, std::vector<LexerState>& line_states,
                           const Text& old_text, const Text& new_text,
                           std::span<const TextEdit> edits);

} // namespace cind
