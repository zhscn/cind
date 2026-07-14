#include "tui/terminal.hpp"

#include <cerrno>
#include <stdexcept>

#include <sys/ioctl.h>
#include <unistd.h>

namespace cind::tui {

namespace {
constexpr int kEof = -2;
}

Terminal::Terminal() {
    if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
        throw std::runtime_error("not a terminal");
    }
    if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
        throw std::runtime_error("tcgetattr failed");
    }
    termios raw = saved_;
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    // TCSANOW, not TCSAFLUSH: type-ahead (and scripted input) must survive
    // entering raw mode.
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        throw std::runtime_error("tcsetattr failed");
    }
    queue("\x1b[?1049h"); // alternate screen
    queue("\x1b[?25l");   // hide cursor while drawing; render re-shows it
    flush();
}

Terminal::~Terminal() {
    out_.clear();
    queue("\x1b[?25h");
    queue("\x1b[?1049l");
    flush();
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
}

TermSize Terminal::size() const {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        return TermSize{ws.ws_row, ws.ws_col};
    }
    return TermSize{};
}

void Terminal::flush() {
    std::string_view rest = out_;
    while (!rest.empty()) {
        const ssize_t n = ::write(STDOUT_FILENO, rest.data(), rest.size());
        if (n <= 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        rest.remove_prefix(static_cast<std::size_t>(n));
    }
    out_.clear();
}

// blocking=false uses a short timeout — used for the bytes after ESC, where
// "nothing follows" means a lone Escape key.
int Terminal::read_byte(bool blocking) {
    termios cur{};
    if (!blocking) {
        tcgetattr(STDIN_FILENO, &cur);
        termios tmp = cur;
        tmp.c_cc[VMIN] = 0;
        tmp.c_cc[VTIME] = 1; // 100ms
        tcsetattr(STDIN_FILENO, TCSANOW, &tmp);
    }
    unsigned char byte = 0;
    const ssize_t n = ::read(STDIN_FILENO, &byte, 1);
    if (!blocking) {
        tcsetattr(STDIN_FILENO, TCSANOW, &cur);
    }
    if (n == 1) {
        return byte;
    }
    return n == 0 && blocking ? kEof : -1; // 0 on a timed read is just a timeout
}

Key Terminal::read_escape_sequence() {
    const int b1 = read_byte(false);
    if (b1 == -1) {
        return Key{KeyKind::Escape, 0, {}};
    }
    if (b1 != '[' && b1 != 'O') {
        return Key{KeyKind::None, 0, {}};
    }
    const int b2 = read_byte(false);
    switch (b2) {
    case 'A': return Key{KeyKind::Up, 0, {}};
    case 'B': return Key{KeyKind::Down, 0, {}};
    case 'C': return Key{KeyKind::Right, 0, {}};
    case 'D': return Key{KeyKind::Left, 0, {}};
    case 'H': return Key{KeyKind::Home, 0, {}};
    case 'F': return Key{KeyKind::End, 0, {}};
    default: break;
    }
    if (b2 >= '0' && b2 <= '9') {
        const int b3 = read_byte(false);
        if (b3 == '~') {
            switch (b2) {
            case '1':
            case '7': return Key{KeyKind::Home, 0, {}};
            case '4':
            case '8': return Key{KeyKind::End, 0, {}};
            case '3': return Key{KeyKind::Delete, 0, {}};
            case '5': return Key{KeyKind::PageUp, 0, {}};
            case '6': return Key{KeyKind::PageDown, 0, {}};
            default: break;
            }
        }
    }
    return Key{KeyKind::None, 0, {}};
}

Key Terminal::read_key() {
    const int b = read_byte(true);
    if (b == kEof) {
        return Key{KeyKind::Eof, 0, {}};
    }
    if (b == -1) {
        return Key{KeyKind::None, 0, {}};
    }
    if (b == 0x1b) {
        return read_escape_sequence();
    }
    if (b == '\r' || b == '\n') {
        return Key{KeyKind::Enter, 0, {}};
    }
    if (b == '\t') {
        return Key{KeyKind::Tab, 0, {}};
    }
    if (b == 127 || b == 8) {
        return Key{KeyKind::Backspace, 0, {}};
    }
    if (b < 0x20) {
        return Key{KeyKind::Ctrl, static_cast<char>('a' + b - 1), {}};
    }
    std::string text(1, static_cast<char>(b));
    if (b >= 0x80) {
        // UTF-8: sequence length from the lead byte, then the continuations.
        int extra = 0;
        if ((b & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((b & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((b & 0xF8) == 0xF0) {
            extra = 3;
        }
        for (int i = 0; i < extra; ++i) {
            const int c = read_byte(false);
            if (c == -1) {
                break;
            }
            text.push_back(static_cast<char>(c));
        }
    }
    return Key{KeyKind::Char, static_cast<char>(b), std::move(text)};
}

} // namespace cind::tui
