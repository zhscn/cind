#pragma once

#include "cpp_lexer/lexer_state.hpp"
#include "cpp_lexer/token.hpp"

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

// Full-file lossless lex. Never fails; unrecognized bytes become Invalid
// tokens and unterminated constructs are flagged, not dropped.
LexOutput lex(std::string_view text);

} // namespace cind
