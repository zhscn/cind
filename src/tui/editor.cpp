#include "tui/editor.hpp"

#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "cpp_lexer/lexer.hpp"
#include "editor/editor_application.hpp"
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
        : application_({.path = path,
                        .initial_text = std::move(initial),
                        .style = style,
                        .style_origin = std::move(style_origin),
                        .initial_line = initial_line}),
          session_(application_.session()), basic_commands_(application_.basic_commands()),
          search_commands_(application_.search_commands()),
          command_loop_(application_.command_loop()), style_origin_(application_.style_origin()),
          message_(application_.message()) {
        register_commands();
        application_.refresh_default_keymap();
        const DocumentSnapshot snap = session_.snapshot();
        const Text& text = snap.content();
        message_ = std::format("read {} lines", reported_lines(text));
    }

    int run() {
        while (!application_.should_quit()) {
            (void)application_.poll_background_work();
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

        if (key.kind == KeyKind::Char) {
            const bool minibuffer_active = command_loop_.minibuffer_active();
            application_.insert_text(key.text);
            command_keep_message_ = minibuffer_active;
        } else if (key.kind == KeyKind::Eof) {
            application_.request_quit(true);
        } else if (const std::optional<KeyStroke> stroke = normalize_key(key)) {
            message_.clear();
            const bool handled = application_.handle_key(*stroke, text_rows());
            command_keep_message_ =
                handled && (!message_.empty() || command_loop_.minibuffer_active() ||
                            !command_loop_.pending_sequence().empty());
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
            application_.runtime().commands().define(
                std::move(name),
                [execute = std::move(execute)](CommandContext&,
                                               const CommandInvocation&) mutable -> CommandResult {
                    execute();
                    return CommandCompleted{};
                });
        };

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

    bool dirty() const { return application_.dirty(); }

    const std::string& path() const { return application_.path(); }

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
        application_.mark_saved(session_.snapshot().content());
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
            application_.runtime().buffers().set_resource(application_.buffer_id(), target,
                                                          BufferKind::File);
            application_.runtime().buffers().rename(
                application_.buffer_id(), std::filesystem::path(*target).filename().string());
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
        if (signs_rev_ != rev || signs_gen_ != application_.save_generation()) {
            signs_ = ui::line_signs(session_.buffer().save_point(), session_.snapshot().content());
            signs_rev_ = rev;
            signs_gen_ = application_.save_generation();
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
                                                    .last_key = application_.last_key(),
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

    EditorApplication application_;
    EditSession& session_;
    BasicEditorCommands& basic_commands_;
    SearchCommands& search_commands_;
    CommandLoop& command_loop_;
    const std::string& style_origin_;
    std::string& message_;
    Terminal term_;

    ui::LineSigns signs_;
    RevisionId signs_rev_ = static_cast<RevisionId>(-1);
    std::uint32_t signs_gen_ = static_cast<std::uint32_t>(-1);
    bool command_keep_message_ = false;
    std::vector<TextRange> expand_stack_;
    std::string kill_slot_;
    bool prompt_active_ = false;
    std::string prompt_label_;
    std::string prompt_input_;
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
