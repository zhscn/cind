#pragma once

#include <string>

namespace cind {

// Cross-line lexical state at a line start. Recorded per line by the full
// lexer; the future incremental lexer's convergence check ("stop when tokens
// and the outgoing state match") consumes the same structure.
struct LexerState {
    bool inside_block_comment = false;
    bool inside_raw_string = false;
    bool preprocessor_continuation = false;
    std::string raw_delimiter; // valid when inside_raw_string

    friend bool operator==(const LexerState&, const LexerState&) = default;
};

} // namespace cind
