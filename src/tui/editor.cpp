#include "tui/editor.hpp"

#include "cli/session.hpp"
#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "cpp_lexer/lexer.hpp"
#include "syntax/structure.hpp"
#include "tui/terminal.hpp"
#include "ui/ansi_renderer.hpp"
#include "ui/char_width.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace cind::tui {

namespace {

bool is_utf8_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

// Key caption for the status line (screenkey-style, so recordings show
// which key produced each reaction).
std::string describe_key(const Key& key) {
    switch (key.kind) {
    case KeyKind::Char:
        return key.text;
    case KeyKind::Ctrl:
        if (key.ch == ' ') {
            return "Ctrl-Space";
        }
        return std::format("Ctrl-{}",
                           static_cast<char>(std::toupper(static_cast<unsigned char>(key.ch))));
    case KeyKind::Alt:
        return std::format("Alt-{}", key.ch);
    case KeyKind::Enter:
        return "Enter";
    case KeyKind::Tab:
        return "Tab";
    case KeyKind::Backspace:
        return "Backspace";
    case KeyKind::Delete:
        return "Delete";
    case KeyKind::Up:
        return "Up";
    case KeyKind::Down:
        return "Down";
    case KeyKind::Left:
        return "Left";
    case KeyKind::Right:
        return "Right";
    case KeyKind::Home:
        return "Home";
    case KeyKind::End:
        return "End";
    case KeyKind::PageUp:
        return "PgUp";
    case KeyKind::PageDown:
        return "PgDn";
    case KeyKind::Escape:
        return "Esc";
    case KeyKind::Eof:
    case KeyKind::None:
        return {};
    }
    return {};
}

class Editor {
public:
    Editor(std::string path, std::string initial, CppIndentStyle style, std::string style_origin,
           std::uint32_t initial_line)
        : path_(std::move(path)), session_(std::move(initial), style),
          style_origin_(std::move(style_origin)), saved_text_(session_.snapshot().content()) {
        const DocumentSnapshot snap = session_.snapshot();
        const Text& text = snap.content();
        message_ = std::format("read {} lines", reported_lines(text));
        if (initial_line > 0) {
            const std::uint32_t line = std::min(initial_line - 1, text.line_count() - 1);
            session_.set_caret(text.line_start(line));
        }
    }

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

    int text_width() const {
        const std::uint32_t lines = session_.snapshot().content().line_count();
        return std::max(1, term_.size().cols - ui::text_area_column(lines));
    }

    int tab_width() const { return session_.style().tab_width; }

    // ---- caret movement ---------------------------------------------------

    void move_vertical(int delta) {
        const DocumentSnapshot snap_keepalive = session_.snapshot();
        const Text& text = snap_keepalive.content();
        const LinePosition pos = text.position(session_.caret());
        if (goal_col_ < 0) {
            goal_col_ = ui::display_column(text, session_.caret(), tab_width());
        }
        const auto line_count = static_cast<int>(text.line_count());
        const int target = std::clamp(static_cast<int>(pos.line) + delta, 0, line_count - 1);
        session_.set_caret(ui::offset_at_display_column(text, static_cast<std::uint32_t>(target),
                                                        goal_col_, tab_width()));
    }

    // ---- editing ----------------------------------------------------------

    void handle_key(const Key& key) {
        const DocumentSnapshot snap_keepalive = session_.snapshot();
        const Text& text = snap_keepalive.content();
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
        case KeyKind::Enter:
            session_.enter();
            break;
        case KeyKind::Tab: {
            IndentDecision decision = session_.indent();
            message_ = std::format("indent: {}", format_role_name(decision.role));
            keep_message = true;
            break;
        }
        case KeyKind::Backspace:
            soft_delete(false);
            break;
        case KeyKind::Delete:
            soft_delete(true);
            break;
        case KeyKind::Left:
            session_.set_caret(ui::previous_code_point(text, caret));
            break;
        case KeyKind::Right:
            session_.set_caret(ui::next_code_point(text, caret));
            break;
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
        case KeyKind::Ctrl:
            handle_ctrl(key.ch, keep_message);
            break;
        case KeyKind::Alt:
            handle_alt(key.ch, keep_message);
            break;
        case KeyKind::Eof:
            quit_ = true;
            break;
        case KeyKind::Escape:
        case KeyKind::None:
            keep_message = true;
            break;
        }

        if (!keep_goal && !keep_vertical_goal_) {
            goal_col_ = -1;
        }
        keep_vertical_goal_ = false;
        if (!keep_message) {
            message_.clear();
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
        const SyntaxTree& tree = session_.analysis().tree;
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
        case 'g':
            command_goto_line();
            break;
        case 'n':
            search_move(last_search_, true);
            break;
        case 'p':
            search_move(last_search_, false);
            break;
        case '%':
            command_replace();
            break;
        case '?':
            show_help();
            break;
        case 'v':
            move_vertical(-text_rows());
            keep_vertical_goal_ = true;
            break;
        default:
            message_.clear();
            break;
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
        const TextOffset target = forward ? caret : ui::previous_code_point(text, caret);
        const char c = text.byte_at(target);
        auto is_open = [](char ch) { return ch == '(' || ch == '[' || ch == '{'; };
        auto is_close = [](char ch) { return ch == ')' || ch == ']' || ch == '}'; };
        auto partner = [](char ch) {
            switch (ch) {
            case '(':
                return ')';
            case '[':
                return ']';
            case '{':
                return '}';
            case ')':
                return '(';
            case ']':
                return '[';
            default:
                return '{';
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
                session_.erase(
                    TextRange{TextOffset{target.value - 1}, TextOffset{target.value + 1}});
                return;
            }
            session_.set_caret(forward ? TextOffset{target.value + 1} : target);
            message_ = "soft delete: pair not empty (moved over)";
            return;
        }
        if (c == '"' || c == '\'') {
            const char other =
                forward ? (target.value >= 1 ? text.byte_at(TextOffset{target.value - 1}) : '\0')
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
        session_.erase(forward ? TextRange{caret, ui::next_code_point(text, caret)}
                               : TextRange{ui::previous_code_point(text, caret), caret});
    }

    void handle_ctrl(char ch, bool& keep_message) {
        keep_message = true;
        switch (ch) {
        case 'q':
            command_quit();
            break;
        case 's':
            save_to(path_);
            break;
        case 'o':
            command_save_as();
            break;
        case 'f':
            command_search();
            break;
        case 'c':
            command_position();
            break;
        case 'g': // cancel: mark, pending states, message
            mark_.reset();
            expand_stack_.clear();
            message_ = "cancelled";
            break;
        case 'a':
            session_.set_caret(session_.snapshot().content().line_start(
                session_.snapshot().content().position(session_.caret()).line));
            break;
        case 'e':
            session_.set_caret(session_.snapshot().content().line_content_end(
                session_.snapshot().content().position(session_.caret()).line));
            break;
        case 'n':
            move_vertical(1);
            keep_vertical_goal_ = true;
            break;
        case 'p':
            move_vertical(-1);
            keep_vertical_goal_ = true;
            break;
        case 'v':
            move_vertical(text_rows());
            keep_vertical_goal_ = true;
            break;
        case 'z':
            message_ = session_.undo() ? "undo" : "nothing to undo";
            break;
        case 'r':
            message_ = session_.redo() ? "redo" : "nothing to redo";
            break;
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
            const SyntaxTree& tree = session_.analysis().tree;
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
        case 'l':
            break; // redraw happens every loop anyway
        default:
            message_.clear();
            break;
        }
    }

    bool dirty() const { return diff_edit(saved_text_, session_.snapshot().content()).has_value(); }

    // ---- minibuffer -------------------------------------------------------

    // Modal one-line prompt on the message line. Enter accepts, Ctrl-G or
    // Escape cancels. The input supports UTF-8 typing and backspace.
    std::optional<std::string> prompt(std::string label, std::string initial = {}) {
        prompt_label_ = std::move(label);
        prompt_input_ = std::move(initial);
        prompt_active_ = true;
        std::optional<std::string> result;
        while (true) {
            render();
            const Key key = term_.read_key();
            if (key.kind == KeyKind::Enter) {
                result = prompt_input_;
                break;
            }
            if (key.kind == KeyKind::Escape || key.kind == KeyKind::Eof ||
                (key.kind == KeyKind::Ctrl && key.ch == 'g')) {
                break;
            }
            if (key.kind == KeyKind::Char) {
                prompt_input_ += key.text;
            } else if (key.kind == KeyKind::Backspace && !prompt_input_.empty()) {
                std::size_t n = prompt_input_.size() - 1;
                while (n > 0 && is_utf8_continuation(prompt_input_[n])) {
                    --n;
                }
                prompt_input_.resize(n);
            }
        }
        prompt_active_ = false;
        return result;
    }

    // Single-key modal question; returns the lowercased key, or 0 on cancel.
    char ask(std::string label) {
        prompt_label_ = std::move(label);
        prompt_input_.clear();
        prompt_active_ = true;
        char answer = 0;
        while (true) {
            render();
            const Key key = term_.read_key();
            if (key.kind == KeyKind::Escape || key.kind == KeyKind::Eof ||
                (key.kind == KeyKind::Ctrl && key.ch == 'g')) {
                break;
            }
            if (key.kind == KeyKind::Char && key.text.size() == 1) {
                answer = static_cast<char>(std::tolower(static_cast<unsigned char>(key.text[0])));
                break;
            }
        }
        prompt_active_ = false;
        return answer;
    }

    // ---- search / goto ----------------------------------------------------

    // Moves the caret to the next occurrence of `needle`; wraps around and
    // says so. `forward` chooses the direction, starting just past the caret.
    void search_move(const std::string& needle, bool forward) {
        if (needle.empty()) {
            return;
        }
        const DocumentSnapshot snap_keepalive = session_.snapshot();
        const Text& text = snap_keepalive.content();
        const std::string hay = text.to_string(); // user-initiated; fine
        const std::size_t caret = session_.caret().value;
        std::size_t found = std::string::npos;
        bool wrapped = false;
        if (forward) {
            found = hay.find(needle, std::min(caret + 1, hay.size()));
            if (found == std::string::npos) {
                found = hay.find(needle);
                wrapped = found != std::string::npos;
            }
        } else {
            if (caret > 0) {
                found = hay.rfind(needle, caret - 1); // match start strictly before caret
            }
            if (found == std::string::npos) {
                found = hay.rfind(needle);
                wrapped = found != std::string::npos;
            }
        }
        if (found == std::string::npos) {
            message_ = std::format("\"{}\" not found", needle);
            return;
        }
        session_.set_caret(TextOffset{static_cast<std::uint32_t>(found)});
        message_ = wrapped ? "search wrapped" : std::string();
    }

    void command_search() {
        std::optional<std::string> input =
            prompt(last_search_.empty() ? "search: " : std::format("search [{}]: ", last_search_));
        if (!input) {
            message_ = "cancelled";
            return;
        }
        if (!input->empty()) {
            last_search_ = *input;
        }
        search_move(last_search_, true);
    }

    void command_replace() {
        std::optional<std::string> from = prompt("replace: ", last_search_);
        if (!from || from->empty()) {
            message_ = "cancelled";
            return;
        }
        last_search_ = *from;
        std::optional<std::string> to = prompt(std::format("replace \"{}\" with: ", *from));
        if (!to) {
            message_ = "cancelled";
            return;
        }
        // Walk matches from the caret, asking per match: y replace, n skip,
        // a all remaining, q stop.
        int replaced = 0;
        bool all = false;
        while (true) {
            const DocumentSnapshot snap_keepalive = session_.snapshot();
            const Text& text = snap_keepalive.content();
            const std::string hay = text.to_string();
            const std::size_t at = hay.find(*from, session_.caret().value);
            if (at == std::string::npos) {
                break;
            }
            session_.set_caret(TextOffset{static_cast<std::uint32_t>(at)});
            char answer = 'y';
            if (!all) {
                render();
                answer = ask(std::format("replace this match? (y/n/a/q) "));
                if (answer == 'q' || answer == 0) {
                    break;
                }
                if (answer == 'a') {
                    all = true;
                }
            }
            if (all || answer == 'y') {
                const auto match_start = static_cast<std::uint32_t>(at);
                session_.erase(make_range(match_start,
                                          match_start + static_cast<std::uint32_t>(from->size())));
                session_.insert_text(*to);
                ++replaced;
            } else {
                session_.set_caret(TextOffset{static_cast<std::uint32_t>(at + from->size())});
            }
        }
        message_ = std::format("replaced {} occurrence{}", replaced, replaced == 1 ? "" : "s");
    }

    void command_goto_line() {
        std::optional<std::string> input = prompt("go to line[,column]: ");
        if (!input || input->empty()) {
            message_ = "cancelled";
            return;
        }
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        const char* p = input->c_str();
        char* rest = nullptr;
        line = static_cast<std::uint32_t>(std::strtoul(p, &rest, 10));
        if (rest != nullptr && (*rest == ',' || *rest == ':')) {
            column = static_cast<std::uint32_t>(std::strtoul(rest + 1, nullptr, 10));
        }
        if (line == 0) {
            message_ = "invalid line number";
            return;
        }
        const DocumentSnapshot snap_keepalive = session_.snapshot();
        const Text& text = snap_keepalive.content();
        const std::uint32_t target = std::min(line - 1, text.line_count() - 1);
        TextOffset offset = text.line_start(target);
        if (column > 1) {
            offset = ui::offset_at_display_column(text, target, static_cast<int>(column - 1),
                                                  tab_width());
        }
        session_.set_caret(offset);
    }

    void command_position() {
        const DocumentSnapshot snap_keepalive = session_.snapshot();
        const Text& text = snap_keepalive.content();
        const TextOffset caret = session_.caret();
        const LinePosition pos = text.position(caret);
        const std::uint32_t lines = text.line_count();
        const std::uint32_t bytes = text.size_bytes();
        auto pct = [](std::uint64_t a, std::uint64_t b) {
            return b == 0 ? 100 : static_cast<int>(a * 100 / b);
        };
        message_ =
            std::format("line {}/{} ({}%), col {}, byte {}/{} ({}%)", pos.line + 1, lines,
                        pct(pos.line + 1, lines), ui::display_column(text, caret, tab_width()) + 1,
                        caret.value, bytes, pct(caret.value, bytes));
    }

    // ---- saving / quitting --------------------------------------------------

    // Line count the way nano reports it: a trailing newline does not start
    // a new line.
    static std::uint32_t reported_lines(const Text& text) {
        const std::uint32_t lines = text.line_count();
        if (lines > 1 && text.line_range(lines - 1).empty()) {
            return lines - 1;
        }
        return lines;
    }

    bool save_to(const std::string& target) {
        if (std::error_code ec = save_file_atomically(target, session_.snapshot().content())) {
            message_ = std::format("save failed: {}", ec.message());
            return false;
        }
        saved_text_ = session_.snapshot().content();
        ++save_gen_; // invalidate the change-sign cache
        message_ = std::format("wrote {} lines to {}", reported_lines(saved_text_), target);
        return true;
    }

    void command_save_as() {
        std::optional<std::string> target = prompt("write file: ", path_);
        if (!target || target->empty()) {
            message_ = "cancelled";
            return;
        }
        if (save_to(*target)) {
            path_ = *target;
        }
    }

    void command_quit() {
        if (!dirty()) {
            quit_ = true;
            return;
        }
        const char answer = ask("save modified buffer? (y/n, Ctrl-G cancel) ");
        if (answer == 'y') {
            quit_ = save_to(path_);
        } else if (answer == 'n') {
            quit_ = true;
        } else {
            message_ = "cancelled";
        }
    }

    void show_help() {
        const TermSize size = term_.size();
        term_.queue("\x1b[?25l\x1b[H\x1b[2J");
        static constexpr std::string_view kHelp[] = {
            "cind editor — keys",
            "",
            "  movement    arrows, PgUp/PgDn, Home/End",
            "              C-a/C-e line start/end   C-n/C-p next/prev line",
            "              C-v/M-v page down/up",
            "  editing     printable keys (typed-char pipeline, on-typing reindent)",
            "              Enter (structural newline+indent)   Tab reindent line",
            "              Backspace/Delete soft delete (balanced pairs step over)",
            "  kill/yank   C-k soft kill to EOL   C-w kill region   M-w copy   C-y yank",
            "  region      C-SPC set/clear mark   M-h expand   M-j shrink",
            "  structure   M-f/M-b sexp forward/back   M-u up to enclosing open",
            "  search      C-f find   M-n/M-p repeat next/prev   M-% query replace",
            "  files       C-s save   C-o write as   C-q quit",
            "  misc        M-g goto line   C-c cursor position   C-z/C-r undo/redo",
            "              C-g cancel   M-? this help",
            "",
            "press any key to continue",
        };
        int row = 0;
        for (std::string_view line : kHelp) {
            if (row++ >= size.rows - 1) {
                break;
            }
            term_.queue(line);
            term_.queue("\x1b[K\r\n");
        }
        term_.flush();
        term_.read_key();
    }

    // ---- rendering --------------------------------------------------------

    const TokenBuffer& tokens() { return session_.analysis().tree.tokens(); }

    // Unsaved-change signs, cached per (revision, save generation): the
    // structural diff makes a miss O(changed bytes + log n).
    const ui::LineSigns& signs() {
        const RevisionId rev = session_.snapshot().revision();
        if (signs_rev_ != rev || signs_gen_ != save_gen_) {
            signs_ = ui::line_signs(saved_text_, session_.snapshot().content());
            signs_rev_ = rev;
            signs_gen_ = save_gen_;
        }
        return signs_;
    }

    ui::Scene compose() {
        const DocumentSnapshot snap = session_.snapshot();
        const TermSize size = term_.size();
        const std::string echo_text =
            prompt_active_
                ? prompt_label_ + prompt_input_
                : (message_.empty() ? "C-s save  C-o write-as  C-q quit  C-f find  M-g goto  "
                                      "C-z/C-r undo  C-k kill  C-y yank  C-SPC mark  M-f/b sexp  "
                                      "M-? help"
                                    : message_);
        std::optional<int> echo_cursor;
        if (prompt_active_) {
            echo_cursor = ui::display_width(prompt_label_) + ui::display_width(prompt_input_);
        }
        return ui::compose_editor_scene({.text = snap.content(),
                                         .tokens = tokens(),
                                         .signs = signs(),
                                         .caret = session_.caret(),
                                         .selection = selection(),
                                         .rows = size.rows,
                                         .cols = size.cols,
                                         .tab_width = tab_width(),
                                         .path = path_,
                                         .dirty = dirty(),
                                         .revision = snap.revision(),
                                         .style_origin = style_origin_,
                                         .last_key = last_key_,
                                         .echo = echo_text,
                                         .echo_cursor_column = echo_cursor},
                                        viewport_);
    }

    void render() {
        term_.queue(ui::render_ansi(compose()));
        term_.flush();
    }

    std::string path_;
    EditSession session_;
    std::string style_origin_;
    Text saved_text_;
    Terminal term_;

    ui::EditorViewport viewport_;
    int goal_col_ = -1;
    std::uint32_t save_gen_ = 0;
    ui::LineSigns signs_;
    RevisionId signs_rev_ = static_cast<RevisionId>(-1);
    std::uint32_t signs_gen_ = static_cast<std::uint32_t>(-1);
    bool keep_vertical_goal_ = false;
    std::string message_;
    std::string last_key_;
    std::optional<TextOffset> mark_;
    std::vector<TextRange> expand_stack_;
    std::string kill_slot_;
    std::string last_search_;
    bool prompt_active_ = false;
    std::string prompt_label_;
    std::string prompt_input_;
    bool quit_ = false;
};

} // namespace

int run_editor(const std::string& path, std::uint32_t initial_line) {
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
        Editor editor(path, std::move(initial), style, std::move(style_origin), initial_line);
        return editor.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "indent-core: %s\n", e.what());
        return 1;
    }
}

} // namespace cind::tui
