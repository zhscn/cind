#include "tui/editor.hpp"

#include "cli/session.hpp"
#include "cli/style_loader.hpp"
#include "cpp_lexer/lexer.hpp"
#include "syntax/structure.hpp"
#include "tui/terminal.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace cind::tui {

namespace {

bool is_continuation_byte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// SGR color per token kind — layer-1 highlighting straight off the lexer.
std::string_view token_color(const Token& token) {
    if (has_flag(token.flags, LexicalFlags::PreprocessorLine)) {
        return "\x1b[33m"; // whole directive line: yellow
    }
    switch (token.kind) {
    case TokenKind::LineComment:
    case TokenKind::BlockComment: return "\x1b[90m";
    case TokenKind::StringLiteral:
    case TokenKind::RawStringLiteral:
    case TokenKind::CharacterLiteral: return "\x1b[32m";
    case TokenKind::Number: return "\x1b[35m";
    case TokenKind::NamespaceKw:
    case TokenKind::ClassKw:
    case TokenKind::StructKw:
    case TokenKind::EnumKw:
    case TokenKind::UnionKw:
    case TokenKind::SwitchKw:
    case TokenKind::CaseKw:
    case TokenKind::DefaultKw:
    case TokenKind::PublicKw:
    case TokenKind::ProtectedKw:
    case TokenKind::PrivateKw:
    case TokenKind::IfKw:
    case TokenKind::ElseKw:
    case TokenKind::ForKw:
    case TokenKind::WhileKw:
    case TokenKind::DoKw:
    case TokenKind::ReturnKw:
    case TokenKind::TemplateKw:
    case TokenKind::OperatorKw: return "\x1b[1;34m";
    default: return {};
    }
}

// Key caption for the status line (screenkey-style, so recordings show
// which key produced each reaction).
std::string describe_key(const Key& key) {
    switch (key.kind) {
    case KeyKind::Char: return key.text;
    case KeyKind::Ctrl:
        if (key.ch == ' ') {
            return "Ctrl-Space";
        }
        return std::format("Ctrl-{}",
                           static_cast<char>(std::toupper(static_cast<unsigned char>(key.ch))));
    case KeyKind::Alt: return std::format("Alt-{}", key.ch);
    case KeyKind::Enter: return "Enter";
    case KeyKind::Tab: return "Tab";
    case KeyKind::Backspace: return "Backspace";
    case KeyKind::Delete: return "Delete";
    case KeyKind::Up: return "Up";
    case KeyKind::Down: return "Down";
    case KeyKind::Left: return "Left";
    case KeyKind::Right: return "Right";
    case KeyKind::Home: return "Home";
    case KeyKind::End: return "End";
    case KeyKind::PageUp: return "PgUp";
    case KeyKind::PageDown: return "PgDn";
    case KeyKind::Escape: return "Esc";
    case KeyKind::Eof:
    case KeyKind::None: return {};
    }
    return {};
}

int caption_width(std::string_view s) {
    int width = 0;
    for (char c : s) {
        if (!is_continuation_byte(c)) {
            width += static_cast<unsigned char>(c) < 0x80 ? 1 : 2; // CJK 约两格
        }
    }
    return width;
}

// Atomic save per buffer.md §6: temp file, fsync, rename, fsync the parent.
std::error_code save_atomically(const std::string& path, const Text& content) {
    namespace fs = std::filesystem;
    const fs::path target(path);
    const fs::path tmp = target.string() + ".cind-tmp";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return std::error_code(errno, std::generic_category());
    }
    for (TextCursor cursor(content); !cursor.at_end(); cursor.advance_chunk()) {
        std::string_view chunk = cursor.chunk();
        while (!chunk.empty()) {
            const ssize_t n = ::write(fd, chunk.data(), chunk.size());
            if (n <= 0) {
                const int err = errno;
                ::close(fd);
                ::unlink(tmp.c_str());
                return std::error_code(err, std::generic_category());
            }
            chunk.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        return std::error_code(err, std::generic_category());
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        ::unlink(tmp.c_str());
        return ec;
    }
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return {};
}

class Editor {
public:
    Editor(std::string path, std::string initial, CppIndentStyle style, std::string style_origin)
        : path_(std::move(path)), session_(std::move(initial), style),
          style_origin_(std::move(style_origin)), saved_text_(session_.snapshot().content()) {}

    int run() {
        while (!quit_) {
            render();
            handle_key(term_.read_key());
        }
        return 0;
    }

private:
    // ---- geometry ---------------------------------------------------------

    int text_rows() const { return std::max(1, term_.size().rows - 2); }

    int gutter_width() const {
        const std::uint32_t lines = session_.snapshot().content().line_count();
        int digits = 1;
        for (std::uint32_t n = lines; n >= 10; n /= 10) {
            ++digits;
        }
        return digits + 1; // number + one space
    }

    // Display column of `offset` within its line (tabs expand, code points
    // count 1 column — wide glyphs are out of scope for phase A).
    int display_column(const Text& text, TextOffset offset) const {
        const std::uint32_t start = text.line_start(text.position(offset).line).value;
        int col = 0;
        for (std::uint32_t i = start; i < offset.value; ++i) {
            const char c = text.byte_at(TextOffset{i});
            if (c == '\t') {
                col += tab_width() - col % tab_width();
            } else if (!is_continuation_byte(c)) {
                ++col;
            }
        }
        return col;
    }

    int tab_width() const { return session_.style().tab_width; }

    // ---- caret movement ---------------------------------------------------

    TextOffset prev_code_point(const Text& text, TextOffset offset) const {
        if (offset.value == 0) {
            return offset;
        }
        std::uint32_t p = offset.value - 1;
        while (p > 0 && is_continuation_byte(text.byte_at(TextOffset{p}))) {
            --p;
        }
        return TextOffset{p};
    }

    TextOffset next_code_point(const Text& text, TextOffset offset) const {
        if (offset.value >= text.size_bytes()) {
            return offset;
        }
        std::uint32_t p = offset.value + 1;
        while (p < text.size_bytes() && is_continuation_byte(text.byte_at(TextOffset{p}))) {
            ++p;
        }
        return TextOffset{p};
    }

    // Offset on `line` closest to the goal display column.
    TextOffset offset_at_display_column(const Text& text, std::uint32_t line, int goal) const {
        const TextRange content = text.line_content_range(line);
        std::uint32_t p = content.start.value;
        int col = 0;
        while (p < content.end.value && col < goal) {
            const char c = text.byte_at(TextOffset{p});
            col += c == '\t' ? tab_width() - col % tab_width() : 1;
            p = next_code_point(text, TextOffset{p}).value;
        }
        return TextOffset{p};
    }

    void move_vertical(int delta) {
        const Text& text = session_.snapshot().content();
        const LinePosition pos = text.position(session_.caret());
        if (goal_col_ < 0) {
            goal_col_ = display_column(text, session_.caret());
        }
        const auto line_count = static_cast<int>(text.line_count());
        const int target = std::clamp(static_cast<int>(pos.line) + delta, 0, line_count - 1);
        session_.set_caret(
            offset_at_display_column(text, static_cast<std::uint32_t>(target), goal_col_));
    }

    // ---- editing ----------------------------------------------------------

    void handle_key(const Key& key) {
        const Text& text = session_.snapshot().content();
        const TextOffset caret = session_.caret();
        const RevisionId rev_before = session_.snapshot().revision();
        bool keep_goal = false;
        bool keep_message = false;
        if (std::string caption = describe_key(key); !caption.empty()) {
            last_key_ = std::move(caption);
        }

        switch (key.kind) {
        case KeyKind::Char:
            if (key.text.size() == 1) {
                session_.type_text(key.text); // full typed-char pipeline
            } else {
                session_.insert_text(key.text); // one undo unit per code point
            }
            break;
        case KeyKind::Enter: session_.enter(); break;
        case KeyKind::Tab: {
            IndentDecision decision = session_.indent();
            message_ = std::format("indent: {}", format_role_name(decision.role));
            keep_message = true;
            break;
        }
        case KeyKind::Backspace: soft_delete(false); break;
        case KeyKind::Delete: soft_delete(true); break;
        case KeyKind::Left: session_.set_caret(prev_code_point(text, caret)); break;
        case KeyKind::Right: session_.set_caret(next_code_point(text, caret)); break;
        case KeyKind::Up:
            move_vertical(-1);
            keep_goal = true;
            break;
        case KeyKind::Down:
            move_vertical(1);
            keep_goal = true;
            break;
        case KeyKind::PageUp:
            move_vertical(-text_rows());
            keep_goal = true;
            break;
        case KeyKind::PageDown:
            move_vertical(text_rows());
            keep_goal = true;
            break;
        case KeyKind::Home:
            session_.set_caret(text.line_start(text.position(caret).line));
            break;
        case KeyKind::End:
            session_.set_caret(text.line_content_end(text.position(caret).line));
            break;
        case KeyKind::Ctrl: handle_ctrl(key.ch, keep_message); break;
        case KeyKind::Alt: handle_alt(key.ch, keep_message); break;
        case KeyKind::Eof: quit_ = true; break;
        case KeyKind::Escape:
        case KeyKind::None: keep_message = true; break;
        }

        if (!keep_goal) {
            goal_col_ = -1;
        }
        if (!keep_message) {
            message_.clear();
        }
        if (key.kind != KeyKind::Ctrl || key.ch != 'q') {
            quit_pending_ = false;
        }
        if (session_.snapshot().revision() != rev_before) {
            mark_.reset(); // edits invalidate the selection
            expand_stack_.clear();
        }
    }

    std::optional<TextRange> selection() const {
        if (!mark_) {
            return std::nullopt;
        }
        const TextOffset caret = session_.caret();
        if (*mark_ == caret) {
            return std::nullopt;
        }
        return *mark_ < caret ? TextRange{*mark_, caret} : TextRange{caret, *mark_};
    }

    void kill_range(TextRange range) {
        kill_slot_ = session_.snapshot().substring(range);
        session_.erase(range);
    }

    // puni-style structural commands over the CST.
    void handle_alt(char ch, bool& keep_message) {
        keep_message = true;
        const DocumentSnapshot snap = session_.snapshot();
        const SyntaxTree tree = parse(snap.content());
        switch (ch) {
        case 'f':
            if (auto unit = sexp_forward(tree, session_.caret())) {
                session_.set_caret(unit->end);
            }
            break;
        case 'b':
            if (auto unit = sexp_backward(tree, session_.caret())) {
                session_.set_caret(unit->start);
            }
            break;
        case 'u':
            if (auto list = enclosing_list(tree, session_.caret())) {
                session_.set_caret(list->start);
            }
            break;
        case 'h': { // expand region (repeat to grow)
            TextRange current = selection().value_or(TextRange{session_.caret(), session_.caret()});
            if (auto next = expand_selection(tree, current)) {
                if (selection()) {
                    expand_stack_.push_back(current);
                }
                mark_ = next->start;
                session_.set_caret(next->end);
            }
            break;
        }
        case 'j': // contract region
            if (!expand_stack_.empty()) {
                TextRange prev = expand_stack_.back();
                expand_stack_.pop_back();
                mark_ = prev.start;
                session_.set_caret(prev.end);
            } else {
                mark_.reset();
            }
            break;
        case 'w': // copy region
            if (auto sel = selection()) {
                kill_slot_ = snap.substring(*sel);
                mark_.reset();
                message_ = "copied";
            }
            break;
        default: message_.clear(); break;
        }
    }

    // Soft deletion: bracket pairs and literal quotes are only deleted when
    // the pair is empty; otherwise the caret moves across the delimiter.
    void soft_delete(bool forward) {
        const DocumentSnapshot snap = session_.snapshot();
        const Text& text = snap.content();
        const TextOffset caret = session_.caret();
        if ((forward && caret.value >= text.size_bytes()) || (!forward && caret.value == 0)) {
            return;
        }
        const TextOffset target = forward ? caret : prev_code_point(text, caret);
        const char c = text.byte_at(target);
        auto is_open = [](char ch) { return ch == '(' || ch == '[' || ch == '{'; };
        auto is_close = [](char ch) { return ch == ')' || ch == ']' || ch == '}'; };
        auto partner = [](char ch) {
            switch (ch) {
            case '(': return ')';
            case '[': return ']';
            case '{': return '}';
            case ')': return '(';
            case ']': return '[';
            default: return '{';
            }
        };
        if (is_open(c) || is_close(c)) {
            // Empty adjacent pair: delete both. Otherwise step over.
            if (is_open(c) && target.value + 1 < text.size_bytes() &&
                text.byte_at(TextOffset{target.value + 1}) == partner(c)) {
                session_.erase(TextRange{target, TextOffset{target.value + 2}});
                return;
            }
            if (is_close(c) && target.value >= 1 &&
                text.byte_at(TextOffset{target.value - 1}) == partner(c)) {
                session_.erase(TextRange{TextOffset{target.value - 1},
                                         TextOffset{target.value + 1}});
                return;
            }
            session_.set_caret(forward ? TextOffset{target.value + 1} : target);
            message_ = "soft delete: pair not empty (moved over)";
            return;
        }
        if (c == '"' || c == '\'') {
            const char other = forward ? (target.value >= 1
                                              ? text.byte_at(TextOffset{target.value - 1})
                                              : '\0')
                                       : (target.value + 1 < text.size_bytes()
                                              ? text.byte_at(TextOffset{target.value + 1})
                                              : '\0');
            if (other == c) { // empty literal: delete both quotes
                const std::uint32_t lo = forward ? target.value - 1 : target.value;
                session_.erase(make_range(lo, lo + 2));
                return;
            }
            session_.set_caret(forward ? TextOffset{target.value + 1} : target);
            message_ = "soft delete: literal not empty (moved over)";
            return;
        }
        session_.erase(forward ? TextRange{caret, next_code_point(text, caret)}
                               : TextRange{prev_code_point(text, caret), caret});
    }

    void handle_ctrl(char ch, bool& keep_message) {
        keep_message = true;
        switch (ch) {
        case 'q':
            if (!dirty() || quit_pending_) {
                quit_ = true;
            } else {
                quit_pending_ = true;
                message_ = "unsaved changes — Ctrl-Q again to discard and quit";
            }
            break;
        case 's': {
            if (std::error_code ec = save_atomically(path_, session_.snapshot().content())) {
                message_ = std::format("save failed: {}", ec.message());
            } else {
                saved_text_ = session_.snapshot().content();
                message_ = std::format("saved {}", path_);
            }
            break;
        }
        case 'z': message_ = session_.undo() ? "undo" : "nothing to undo"; break;
        case 'r': message_ = session_.redo() ? "redo" : "nothing to redo"; break;
        case ' ': // set/clear mark
            if (mark_ && *mark_ == session_.caret()) {
                mark_.reset();
                message_ = "mark cleared";
            } else {
                mark_ = session_.caret();
                expand_stack_.clear();
                message_ = "mark set";
            }
            break;
        case 'w': // kill region
            if (auto sel = selection()) {
                kill_range(*sel);
            }
            break;
        case 'k': { // soft kill to end of line (never breaks balance)
            const DocumentSnapshot snap = session_.snapshot();
            const SyntaxTree tree = parse(snap.content());
            TextRange range = soft_kill_end(tree, snap.content(), session_.caret());
            if (!range.empty()) {
                kill_range(range);
            }
            break;
        }
        case 'y': // yank
            if (!kill_slot_.empty()) {
                session_.insert_text(kill_slot_);
            }
            break;
        case 'l': break; // redraw happens every loop anyway
        default: message_.clear(); break;
        }
    }

    bool dirty() const {
        return diff_edit(saved_text_, session_.snapshot().content()).has_value();
    }

    // ---- rendering --------------------------------------------------------

    const LexOutput& tokens() {
        const DocumentSnapshot snap = session_.snapshot();
        if (!lexed_ || lexed_revision_ != snap.revision()) {
            lex_cache_ = lex(snap.content());
            lexed_revision_ = snap.revision();
            lexed_ = true;
        }
        return lex_cache_;
    }

    void render_line(const Text& text, std::uint32_t line, int width) {
        const TextRange content = text.line_content_range(line);
        const LexOutput& lexed = tokens();
        // First token overlapping the line.
        auto it = std::ranges::lower_bound(lexed.tokens, content.start,
                                           [](TextOffset a, TextOffset b) { return a < b; },
                                           [](const Token& t) { return t.range.end; });

        const std::optional<TextRange> sel = selection();
        int col = 0;
        std::string_view active_color;
        bool active_sel = false;
        for (std::uint32_t p = content.start.value; p < content.end.value;) {
            while (it != lexed.tokens.end() && it->range.end.value <= p) {
                ++it;
            }
            std::string_view color;
            if (it != lexed.tokens.end() && it->range.start.value <= p) {
                color = token_color(*it);
            }
            const char c = text.byte_at(TextOffset{p});
            const int cell = c == '\t' ? tab_width() - col % tab_width()
                                       : (is_continuation_byte(c) ? 0 : 1);
            if (col + cell > left_col_ + width) {
                break;
            }
            const bool visible = col >= left_col_ || (cell > 0 && col + cell > left_col_);
            if (visible) {
                if (color != active_color) {
                    term_.queue(active_color.empty() ? "" : "\x1b[0m");
                    term_.queue(color);
                    active_color = color;
                    active_sel = false; // SGR 0 above cleared the selection too
                }
                const bool in_sel = sel && sel->contains(TextOffset{p});
                if (in_sel != active_sel) {
                    term_.queue(in_sel ? "\x1b[7m" : "\x1b[27m");
                    active_sel = in_sel;
                }
                if (c == '\t') {
                    term_.queue(std::string(static_cast<std::size_t>(cell), ' '));
                } else {
                    term_.queue(std::string_view(&c, 1));
                }
            }
            col += cell;
            ++p;
        }
        if (!active_color.empty() || active_sel) {
            term_.queue("\x1b[0m");
        }
    }

    void render() {
        const DocumentSnapshot snap = session_.snapshot();
        const Text& text = snap.content();
        const TermSize size = term_.size();
        const int rows = text_rows();
        const int gutter = gutter_width();
        const int width = std::max(1, size.cols - gutter);

        // Keep the caret inside the viewport.
        const LinePosition caret_pos = text.position(session_.caret());
        if (caret_pos.line < top_line_) {
            top_line_ = caret_pos.line;
        }
        if (caret_pos.line >= top_line_ + static_cast<std::uint32_t>(rows)) {
            top_line_ = caret_pos.line - static_cast<std::uint32_t>(rows) + 1;
        }
        const int caret_col = display_column(text, session_.caret());
        if (caret_col < left_col_) {
            left_col_ = caret_col;
        }
        if (caret_col >= left_col_ + width) {
            left_col_ = caret_col - width + 1;
        }

        term_.queue("\x1b[?25l\x1b[H");
        for (int row = 0; row < rows; ++row) {
            const std::uint32_t line = top_line_ + static_cast<std::uint32_t>(row);
            term_.queue("\x1b[K");
            if (line < text.line_count()) {
                term_.queue(std::format("\x1b[90m{:>{}} \x1b[0m", line + 1, gutter - 1));
                render_line(text, line, width);
            } else {
                term_.queue("\x1b[90m~\x1b[0m");
            }
            term_.queue("\r\n");
        }

        // Status line (reverse video, keystroke caption on the right) +
        // message line.
        std::string status =
            std::format(" {}{}  {}:{}  rev {}  style {} ", path_, dirty() ? " [+]" : "",
                        caret_pos.line + 1, caret_col + 1, snap.revision(), style_origin_);
        const std::string key_caption =
            last_key_.empty() ? std::string() : std::format("key: {} ", last_key_);
        int fill = size.cols - caption_width(status) - caption_width(key_caption);
        if (fill < 0) {
            status.resize(std::max<std::size_t>(
                0, status.size() + static_cast<std::size_t>(fill)));
            fill = 0;
        }
        term_.queue("\x1b[7m");
        term_.queue(status);
        term_.queue(std::string(static_cast<std::size_t>(fill), ' '));
        term_.queue("\x1b[1m");
        term_.queue(key_caption);
        term_.queue("\x1b[0m\r\n\x1b[K");
        term_.queue(message_.empty()
                        ? "C-s save  C-q quit  C-z/C-r undo/redo  C-k kill  C-y yank  C-SPC mark  "
                          "M-f/b sexp  M-u up  M-h/j expand/shrink  Tab indent"
                        : message_);

        // Park the cursor on the caret.
        const int crow = static_cast<int>(caret_pos.line - top_line_) + 1;
        const int ccol = gutter + (caret_col - left_col_) + 1;
        term_.queue(std::format("\x1b[{};{}H\x1b[?25h", crow, ccol));
        term_.flush();
    }

    std::string path_;
    EditSession session_;
    std::string style_origin_;
    Text saved_text_;
    Terminal term_;

    std::uint32_t top_line_ = 0;
    int left_col_ = 0;
    int goal_col_ = -1;
    std::string message_;
    std::string last_key_;
    std::optional<TextOffset> mark_;
    std::vector<TextRange> expand_stack_;
    std::string kill_slot_;
    bool quit_ = false;
    bool quit_pending_ = false;

    bool lexed_ = false;
    RevisionId lexed_revision_ = 0;
    LexOutput lex_cache_;
};

} // namespace

int run_editor(const std::string& path) {
    std::string initial;
    if (std::filesystem::exists(path)) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "indent-core: cannot open %s\n", path.c_str());
            return 1;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        initial = buffer.str();
    }

    CppIndentStyle style;
    std::string style_origin = "llvm (fallback)";
    const auto dir = std::filesystem::absolute(path).parent_path();
    if (auto loaded = load_clang_format_style(dir)) {
        style = loaded->style;
        style_origin = loaded->config_path.filename().string();
    }

    try {
        Editor editor(path, std::move(initial), style, std::move(style_origin));
        return editor.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "indent-core: %s\n", e.what());
        return 1;
    }
}

} // namespace cind::tui
