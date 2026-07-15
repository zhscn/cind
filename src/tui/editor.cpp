#include "tui/editor.hpp"

#include "cli/session.hpp"
#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "cpp_lexer/lexer.hpp"
#include "editor/basic_commands.hpp"
#include "editor/command_loop.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/default_keymap.hpp"
#include "editor/search_commands.hpp"
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
#include <stdexcept>

namespace cind::tui {

namespace {

bool is_utf8_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

BufferId create_file_buffer(EditorRuntime& runtime, const std::string& path, std::string initial) {
    const CppModeRegistration cpp = ensure_cpp_mode(runtime);
    const BufferId buffer = runtime.buffers().create(BufferSpec{.name = {},
                                                                .initial_text = std::move(initial),
                                                                .kind = BufferKind::File,
                                                                .resource_uri = path,
                                                                .read_only = false});
    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), cpp.mode);
    return buffer;
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

std::optional<KeyStroke> normalize_key(const Key& key) {
    switch (key.kind) {
    case KeyKind::Ctrl:
        return KeyStroke::character_key(static_cast<unsigned char>(key.ch), KeyModifier::Control);
    case KeyKind::Alt: {
        KeyModifiers modifiers = KeyModifier::Alt;
        char character = key.ch;
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            modifiers |= KeyModifier::Shift;
        }
        return KeyStroke::character_key(static_cast<unsigned char>(character), modifiers);
    }
    case KeyKind::Enter:
        return KeyStroke::named(KeyCode::Enter);
    case KeyKind::Tab:
        return KeyStroke::named(KeyCode::Tab);
    case KeyKind::Backspace:
        return KeyStroke::named(KeyCode::Backspace);
    case KeyKind::Delete:
        return KeyStroke::named(KeyCode::Delete);
    case KeyKind::Up:
        return KeyStroke::named(KeyCode::Up);
    case KeyKind::Down:
        return KeyStroke::named(KeyCode::Down);
    case KeyKind::Left:
        return KeyStroke::named(KeyCode::Left);
    case KeyKind::Right:
        return KeyStroke::named(KeyCode::Right);
    case KeyKind::Home:
        return KeyStroke::named(KeyCode::Home);
    case KeyKind::End:
        return KeyStroke::named(KeyCode::End);
    case KeyKind::PageUp:
        return KeyStroke::named(KeyCode::PageUp);
    case KeyKind::PageDown:
        return KeyStroke::named(KeyCode::PageDown);
    case KeyKind::Escape:
        return KeyStroke::named(KeyCode::Escape);
    case KeyKind::Char:
    case KeyKind::Eof:
    case KeyKind::None:
        return std::nullopt;
    }
    return std::nullopt;
}

class Editor {
public:
    Editor(const std::string& path, std::string initial, CppIndentStyle style,
           std::string style_origin, std::uint32_t initial_line)
        : buffer_id_(create_file_buffer(runtime_, path, std::move(initial))),
          view_id_(runtime_.views().create(buffer_id_)),
          session_(runtime_, buffer_id_, view_id_, style),
          basic_commands_(runtime_, session_,
                          {.page_rows = [this] { return text_rows(); },
                           .show_message =
                               [this](std::string message) {
                                   message_ = std::move(message);
                                   command_keep_message_ = true;
                               },
                           .edited = {},
                           .caret_moved = {}}),
          search_commands_(runtime_, session_,
                           [this](std::string message) {
                               message_ = std::move(message);
                               command_keep_message_ = true;
                           }),
          command_loop_(runtime_), style_origin_(std::move(style_origin)) {
        register_commands();
        keymap_ = runtime_.keymaps().define("editor.default");
        (void)bind_default_editor_keys(runtime_, keymap_);
        command_loop_.set_keymaps({keymap_});
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

    // ---- editing ----------------------------------------------------------

    void handle_key(const Key& key) {
        const RevisionId rev_before = session_.snapshot().revision();
        command_keep_message_ = false;
        if (std::string caption = describe_key(key); !caption.empty()) {
            last_key_ = std::move(caption);
        }

        if (command_loop_.minibuffer_active()) {
            CommandContext context(runtime_, buffer_id_, view_id_);
            CommandLoopResult result;
            if (key.kind == KeyKind::Char) {
                command_loop_.minibuffer_insert(key.text);
                command_keep_message_ = true;
            } else if (key.kind == KeyKind::Backspace) {
                (void)command_loop_.minibuffer_erase_backward();
                command_keep_message_ = true;
            } else if (key.kind == KeyKind::Enter) {
                result = command_loop_.submit_minibuffer(context);
            } else if (key.kind == KeyKind::Escape ||
                       (key.kind == KeyKind::Ctrl && key.ch == 'g')) {
                result = command_loop_.cancel_minibuffer();
            }
            if (!result.message.empty()) {
                message_ = result.message;
                command_keep_message_ = true;
            }
        } else if (key.kind == KeyKind::Char) {
            basic_commands_.reset_preferred_column();
            if (key.text.size() == 1) {
                session_.type_text(key.text); // full typed-char pipeline
            } else {
                session_.insert_text(key.text); // one undo unit per code point
            }
        } else if (key.kind == KeyKind::Eof) {
            quit_ = true;
        } else if (const std::optional<KeyStroke> stroke = normalize_key(key)) {
            CommandContext context(runtime_, buffer_id_, view_id_);
            const CommandLoopResult result = command_loop_.dispatch(*stroke, context);
            if (result.status == CommandLoopStatus::Prefix ||
                result.status == CommandLoopStatus::Error ||
                result.status == CommandLoopStatus::Disabled ||
                (result.status == CommandLoopStatus::NotHandled && result.consumed)) {
                if (!result.message.empty()) {
                    message_ = result.message;
                }
                command_keep_message_ = true;
            }
        } else {
            command_keep_message_ = true;
        }

        if (!command_keep_message_) {
            message_.clear();
        }
        if (session_.snapshot().revision() != rev_before) {
            session_.clear_selection(); // edits invalidate the selection
            expand_stack_.clear();
        }
    }

    void register_commands() {
        auto define = [this](std::string name, auto execute) {
            runtime_.commands().define(
                std::move(name),
                [execute = std::move(execute)](CommandContext&,
                                               const CommandInvocation&) mutable -> CommandResult {
                    execute();
                    return CommandCompleted{};
                });
        };

        define("application.quit", [this] { handle_ctrl('q', command_keep_message_); });
        define("file.save", [this] { handle_ctrl('s', command_keep_message_); });
        define("file.save-as", [this] { handle_ctrl('o', command_keep_message_); });
        define("editor.position", [this] { handle_ctrl('c', command_keep_message_); });
        define("keyboard.quit", [this] { handle_ctrl('g', command_keep_message_); });
        define("selection.toggle-mark", [this] { handle_ctrl(' ', command_keep_message_); });
        define("edit.kill-region", [this] { handle_ctrl('w', command_keep_message_); });
        define("edit.kill-line", [this] { handle_ctrl('k', command_keep_message_); });
        define("edit.yank", [this] { handle_ctrl('y', command_keep_message_); });
        define("editor.redraw", [this] { handle_ctrl('l', command_keep_message_); });

        define("cursor.forward-expression", [this] { handle_alt('f', command_keep_message_); });
        define("cursor.backward-expression", [this] { handle_alt('b', command_keep_message_); });
        define("cursor.up-list", [this] { handle_alt('u', command_keep_message_); });
        define("selection.expand", [this] { handle_alt('h', command_keep_message_); });
        define("selection.contract", [this] { handle_alt('j', command_keep_message_); });
        define("edit.copy-region", [this] { handle_alt('w', command_keep_message_); });
        define("cursor.goto-line", [this] { handle_alt('g', command_keep_message_); });
        define("search.replace", [this] { handle_alt('%', command_keep_message_); });
        define("help.keys", [this] { handle_alt('?', command_keep_message_); });
    }

    std::optional<TextRange> selection() const { return session_.selection(); }

    void kill_range(TextRange range) {
        kill_slot_ = session_.snapshot().substring(range);
        session_.erase(range);
    }

    // puni-style structural commands over the CST.
    void handle_alt(char ch, bool& keep_message) {
        basic_commands_.reset_preferred_column();
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
                session_.set_selection({.anchor = next->start, .head = next->end});
            }
            break;
        }
        case 'j': // contract region
            if (!expand_stack_.empty()) {
                TextRange prev = expand_stack_.back();
                expand_stack_.pop_back();
                session_.set_selection({.anchor = prev.start, .head = prev.end});
            } else {
                session_.clear_selection();
            }
            break;
        case 'w': // copy region
            if (auto sel = selection()) {
                kill_slot_ = snap.substring(*sel);
                session_.clear_selection();
                message_ = "copied";
            }
            break;
        case 'g':
            command_goto_line();
            break;
        case 'n':
            (void)search_commands_.move(true);
            break;
        case 'p':
            (void)search_commands_.move(false);
            break;
        case '%':
            command_replace();
            break;
        case '?':
            show_help();
            break;
        default:
            message_.clear();
            break;
        }
    }

    void handle_ctrl(char ch, bool& keep_message) {
        basic_commands_.reset_preferred_column();
        keep_message = true;
        switch (ch) {
        case 'q':
            command_quit();
            break;
        case 's':
            save_to(path());
            break;
        case 'o':
            command_save_as();
            break;
        case 'c':
            command_position();
            break;
        case 'g': // cancel: mark, pending states, message
            session_.clear_selection();
            expand_stack_.clear();
            message_ = "cancelled";
            break;
        case ' ': // set/clear mark
            if (session_.mark() && *session_.mark() == session_.caret()) {
                session_.clear_selection();
                message_ = "mark cleared";
            } else {
                session_.set_selection({.anchor = session_.caret(), .head = session_.caret()});
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

    bool dirty() const { return session_.buffer().modified(); }

    const std::string& path() const {
        const std::optional<std::string>& resource = session_.buffer().resource_uri();
        if (!resource) {
            throw std::logic_error("file buffer has no resource URI");
        }
        return *resource;
    }

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

    void command_replace() {
        std::optional<std::string> from =
            prompt("replace: ", std::string(search_commands_.query()));
        if (!from || from->empty()) {
            message_ = "cancelled";
            return;
        }
        search_commands_.set_query(*from);
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
            offset = ui::offset_at_display_column(
                text, {.line = target, .column = static_cast<int>(column - 1)}, tab_width());
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
        session_.buffer().mark_saved(session_.snapshot().content());
        ++save_gen_; // invalidate the change-sign cache
        message_ = std::format("wrote {} lines to {}",
                               reported_lines(session_.buffer().save_point()), target);
        return true;
    }

    void command_save_as() {
        std::optional<std::string> target = prompt("write file: ", path());
        if (!target || target->empty()) {
            message_ = "cancelled";
            return;
        }
        if (save_to(*target)) {
            runtime_.buffers().set_resource(buffer_id_, target, BufferKind::File);
            runtime_.buffers().rename(buffer_id_,
                                      std::filesystem::path(*target).filename().string());
        }
    }

    void command_quit() {
        if (!dirty()) {
            quit_ = true;
            return;
        }
        const char answer = ask("save modified buffer? (y/n, Ctrl-G cancel) ");
        if (answer == 'y') {
            quit_ = save_to(path());
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
            signs_ = ui::line_signs(session_.buffer().save_point(), session_.snapshot().content());
            signs_rev_ = rev;
            signs_gen_ = save_gen_;
        }
        return signs_;
    }

    ui::Scene compose() {
        const DocumentSnapshot snap = session_.snapshot();
        const TermSize size = term_.size();
        const MinibufferState* command_prompt = command_loop_.minibuffer();
        const bool any_prompt = command_prompt != nullptr || prompt_active_;
        const std::string echo_text =
            command_prompt     ? command_prompt->request.prompt + command_prompt->input
            : prompt_active_   ? prompt_label_ + prompt_input_
            : message_.empty() ? "C-s save  C-o write-as  C-q quit  C-f find  M-g goto  "
                                 "C-z/C-r undo  C-k kill  C-y yank  C-SPC mark  M-f/b sexp  "
                                 "M-? help"
                               : message_;
        std::optional<int> echo_cursor;
        if (any_prompt) {
            echo_cursor = ui::display_width(echo_text);
        }
        ViewportState& state = session_.view().viewport();
        ui::EditorViewport viewport{.top_line = state.top_line,
                                    .top_line_offset = state.top_line_offset,
                                    .left_column = state.left_column};
        ui::Scene scene = ui::compose_editor_scene({.text = snap.content(),
                                                    .tokens = tokens(),
                                                    .signs = signs(),
                                                    .caret = session_.caret(),
                                                    .selection = selection(),
                                                    .rows = size.rows,
                                                    .cols = size.cols,
                                                    .tab_width = tab_width(),
                                                    .path = path(),
                                                    .dirty = dirty(),
                                                    .revision = snap.revision(),
                                                    .style_origin = style_origin_,
                                                    .last_key = last_key_,
                                                    .echo = echo_text,
                                                    .echo_cursor_column = echo_cursor},
                                                   viewport);
        state.top_line = viewport.top_line;
        state.top_line_offset = viewport.top_line_offset;
        state.left_column = viewport.left_column;
        return scene;
    }

    void render() {
        term_.queue(ui::render_ansi(compose()));
        term_.flush();
    }

    EditorRuntime runtime_;
    BufferId buffer_id_;
    ViewId view_id_;
    EditSession session_;
    BasicEditorCommands basic_commands_;
    SearchCommands search_commands_;
    CommandLoop command_loop_;
    KeymapId keymap_;
    std::string style_origin_;
    Terminal term_;

    std::uint32_t save_gen_ = 0;
    ui::LineSigns signs_;
    RevisionId signs_rev_ = static_cast<RevisionId>(-1);
    std::uint32_t signs_gen_ = static_cast<std::uint32_t>(-1);
    bool command_keep_message_ = false;
    std::string message_;
    std::string last_key_;
    std::vector<TextRange> expand_stack_;
    std::string kill_slot_;
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
