#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <termios.h>

namespace cind::tui {

struct TermSize {
    int rows = 24;
    int cols = 80;
};

enum class KeyKind : std::uint8_t {
    Char, // printable input; `text` holds the whole UTF-8 sequence
    Ctrl, // control chord; `ch` holds the lowercase letter ('s' for Ctrl-S)
    Alt,  // ESC-prefixed chord; Alt-x and Ctrl-Alt-x both yield `ch` = 'x'
    Enter,
    Tab,
    Backspace,
    Delete,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Escape,
    Eof,  // stdin closed — the caller should exit its loop
    None, // unrecognized or interrupted read
};

struct Key {
    KeyKind kind = KeyKind::None;
    char ch = 0;
    std::string text;
    bool control = false;
};

// OSC 52 requests the terminal emulator to place UTF-8 text in its system
// clipboard. The payload is base64 encoded so terminal control bytes in the
// selected text cannot escape the sequence.
std::string osc52_copy_sequence(std::string_view text);

// Raw-mode terminal (RAII): alternate screen, no echo, no canonical input,
// no signal keys — Ctrl-C/Z/S reach the editor as ordinary keys. Restores
// everything on destruction. Throws std::runtime_error when stdin/stdout is
// not a terminal.
class Terminal {
public:
    Terminal();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    TermSize size() const;
    Key read_key(); // blocking

    // Buffered output: frames are composed off-screen and flushed as one
    // write to avoid tearing.
    void queue(std::string_view s) { out_ += s; }
    void set_clipboard_text(std::string_view text) { queue(osc52_copy_sequence(text)); }
    void flush();

private:
    int read_byte(bool blocking);
    Key read_escape_sequence();

    termios saved_{};
    std::string out_;
};

} // namespace cind::tui
