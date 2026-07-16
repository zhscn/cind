#include "tui/editor.hpp"

#include "cpp_lexer/lexer.hpp"
#include "editor/editor_application.hpp"
#include "tui/terminal.hpp"
#include "ui/ansi_renderer.hpp"
#include "ui/char_width.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <format>
#include <optional>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace cind::tui {

namespace {

bool is_utf8_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

class EventLoopWakeup {
public:
    EventLoopWakeup() {
        int descriptors[2] = {-1, -1};
        if (::pipe(descriptors) != 0) {
            throw std::system_error(errno, std::generic_category(), "cannot create TUI wake pipe");
        }
        read_fd_ = descriptors[0];
        write_fd_ = descriptors[1];
        if (!add_flags(read_fd_) || !add_flags(write_fd_)) {
            const int error = errno;
            ::close(read_fd_);
            ::close(write_fd_);
            throw std::system_error(error, std::generic_category(),
                                    "cannot configure TUI wake pipe");
        }
    }

    ~EventLoopWakeup() {
        ::close(read_fd_);
        ::close(write_fd_);
    }
    EventLoopWakeup(const EventLoopWakeup&) = delete;
    EventLoopWakeup& operator=(const EventLoopWakeup&) = delete;

    void notify() const noexcept {
        constexpr char byte = 1;
        ssize_t result = 0;
        do {
            result = ::write(write_fd_, &byte, 1);
        } while (result < 0 && errno == EINTR);
        (void)result;
    }

    bool wait_for_input() {
        pollfd descriptors[2] = {{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
                                 {.fd = read_fd_, .events = POLLIN, .revents = 0}};
        int result = 0;
        do {
            result = ::poll(descriptors, 2, -1);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            throw std::system_error(errno, std::generic_category(), "TUI input poll failed");
        }
        if ((descriptors[1].revents & static_cast<short>(POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("TUI wake pipe failed");
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            drain();
            return false;
        }
        return (descriptors[0].revents & static_cast<short>(POLLIN | POLLHUP | POLLERR)) != 0;
    }

private:
    static bool add_flags(int descriptor) noexcept {
        const int status_flags = ::fcntl(descriptor, F_GETFL);
        if (status_flags == -1 || ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) == -1) {
            return false;
        }
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        return descriptor_flags != -1 &&
               ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != -1;
    }

    void drain() const noexcept {
        char bytes[64];
        while (::read(read_fd_, bytes, sizeof(bytes)) > 0) {
        }
    }

    int read_fd_ = -1;
    int write_fd_ = -1;
};

std::optional<KeyStroke> normalize_key(const Key& key) {
    switch (key.kind) {
    case KeyKind::Ctrl:
        return KeyStroke::character_key(static_cast<unsigned char>(key.ch), KeyModifier::Control);
    case KeyKind::Alt: {
        KeyModifiers modifiers = KeyModifier::Alt;
        if (key.control) {
            modifiers |= KeyModifier::Control;
        }
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
    Editor(const std::string& path, std::optional<std::string> initial, CppIndentStyle style,
           std::string style_origin, std::uint32_t initial_line)
        : term_(), wakeup_(),
          application_({.path = path,
                        .initial_text = std::move(initial),
                        .style = style,
                        .style_origin = std::move(style_origin),
                        .initial_line = initial_line,
                        .platform_services = {.write_clipboard = [this](std::string_view text)
                                                  -> std::expected<void, std::string> {
                                                  term_.set_clipboard_text(text);
                                                  return {};
                                              },
                                              .read_clipboard = {},
                                              .wake_event_loop = [this] { wakeup_.notify(); }}}),
          search_commands_(application_.search_commands()),
          command_loop_(application_.command_loop()), message_(application_.message()) {
        register_commands();
        application_.refresh_default_keymap();
        const DocumentSnapshot snap = session().snapshot();
        const Text& text = snap.content();
        message_ = std::format("read {} lines", reported_lines(text));
    }

    int run() {
        while (!application_.should_quit()) {
            (void)application_.poll_background_work();
            render();
            handle_key(wait_key());
        }
        return 0;
    }

private:
    EditSession& session() { return application_.session(); }
    const EditSession& session() const { return application_.session(); }

    Key wait_key() {
        while (true) {
            if (wakeup_.wait_for_input()) {
                return term_.read_key();
            }
            if (application_.poll_background_work()) {
                render();
            }
        }
    }

    // ---- geometry ---------------------------------------------------------

    int text_rows() const { return std::max(1, term_.size().rows - 2); }

    int text_width() const {
        const std::uint32_t lines = session().snapshot().content().line_count();
        return std::max(1, term_.size().cols - ui::text_area_column(lines));
    }

    int tab_width() const { return session().style().tab_width; }

    // ---- editing ----------------------------------------------------------

    void handle_key(const Key& key) {
        const RevisionId rev_before = session().snapshot().revision();
        const BufferId buffer_before = application_.buffer_id();
        command_keep_message_ = false;

        if (key.kind == KeyKind::Char) {
            const bool interaction_active = application_.interaction().active();
            if (!interaction_active && !command_loop_.pending_sequence().empty() &&
                key.text.size() == 1) {
                char character = key.text.front();
                KeyModifiers modifiers;
                if (character >= 'A' && character <= 'Z') {
                    character =
                        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                    modifiers |= KeyModifier::Shift;
                }
                const bool handled = application_.handle_key(
                    KeyStroke::character_key(static_cast<unsigned char>(character), modifiers),
                    text_rows());
                command_keep_message_ =
                    handled && (!message_.empty() || !command_loop_.pending_sequence().empty());
            } else if (!interaction_active && !command_loop_.pending_sequence().empty()) {
                message_ = std::format("undefined key: {} {}",
                                       command_loop_.pending_sequence_text(), key.text);
                command_loop_.cancel_pending();
                command_keep_message_ = true;
            } else {
                application_.insert_text(key.text);
                command_keep_message_ = interaction_active;
            }
        } else if (key.kind == KeyKind::Eof) {
            application_.request_quit(true);
        } else if (const std::optional<KeyStroke> stroke = normalize_key(key)) {
            message_.clear();
            const bool handled = application_.handle_key(*stroke, text_rows());
            command_keep_message_ =
                handled && (!message_.empty() || application_.interaction().active() ||
                            !command_loop_.pending_sequence().empty());
        } else {
            command_keep_message_ = true;
        }

        if (!command_keep_message_) {
            message_.clear();
        }
        if (application_.buffer_id() == buffer_before &&
            session().snapshot().revision() != rev_before) {
            session().clear_selection(); // edits invalidate the selection
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

        define("search.replace", [this] {
            command_keep_message_ = true;
            command_replace();
        });
    }

    std::optional<TextRange> selection() const { return session().selection(); }

    bool dirty() const { return application_.dirty(); }

    const std::string& path() const { return application_.path(); }

    // ---- synchronous compatibility prompts -------------------------------

    // Modal one-line prompt on the message line. Enter accepts, Ctrl-G or
    // Escape cancels. The input supports UTF-8 typing and backspace.
    std::optional<std::string> prompt(std::string label, std::string initial = {}) {
        prompt_label_ = std::move(label);
        prompt_input_ = std::move(initial);
        prompt_active_ = true;
        std::optional<std::string> result;
        while (true) {
            render();
            const Key key = wait_key();
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
            const Key key = wait_key();
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
            const DocumentSnapshot snap_keepalive = session().snapshot();
            const Text& text = snap_keepalive.content();
            const std::string hay = text.to_string();
            const std::size_t at = hay.find(*from, session().caret().value);
            if (at == std::string::npos) {
                break;
            }
            session().set_caret(TextOffset{static_cast<std::uint32_t>(at)});
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
                session().erase(make_range(match_start,
                                           match_start + static_cast<std::uint32_t>(from->size())));
                session().insert_text(*to);
                ++replaced;
            } else {
                session().set_caret(TextOffset{static_cast<std::uint32_t>(at + from->size())});
            }
        }
        message_ = std::format("replaced {} occurrence{}", replaced, replaced == 1 ? "" : "s");
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

    // ---- rendering --------------------------------------------------------

    const TokenBuffer& tokens() { return application_.syntax_tokens(); }

    // Unsaved-change signs, cached per (revision, save generation): the
    // structural diff makes a miss O(changed bytes + log n).
    const ui::LineSigns& signs() {
        const RevisionId rev = session().snapshot().revision();
        if (signs_buffer_ != application_.buffer_id() || signs_rev_ != rev ||
            signs_gen_ != application_.save_generation()) {
            signs_ =
                ui::line_signs(session().buffer().save_point(), session().snapshot().content());
            signs_buffer_ = application_.buffer_id();
            signs_rev_ = rev;
            signs_gen_ = application_.save_generation();
        }
        return signs_;
    }

    ui::Scene compose() {
        const DocumentSnapshot snap = session().snapshot();
        const TermSize size = term_.size();
        const InteractionState* interaction = application_.interaction().state();
        const bool any_prompt = interaction != nullptr || prompt_active_;
        const std::string echo_text =
            interaction        ? interaction->request.prompt + interaction->input.text()
            : prompt_active_   ? prompt_label_ + prompt_input_
            : message_.empty() ? "C-x C-s save  C-x C-f open  C-x b buffer  C-x C-c quit  "
                                 "C-s search  M-x commands  C-h b help"
                               : message_;
        std::optional<int> echo_cursor;
        std::optional<std::size_t> echo_cursor_byte;
        if (interaction != nullptr) {
            echo_cursor = ui::display_width(interaction->request.prompt) +
                          ui::display_width(std::string_view(interaction->input.text())
                                                .substr(0, interaction->input.caret()));
            echo_cursor_byte = interaction->request.prompt.size() + interaction->input.caret();
        } else if (any_prompt) {
            echo_cursor = ui::display_width(echo_text);
            echo_cursor_byte = echo_text.size();
        }
        const std::vector<KeyBindingHint> key_hints = application_.pending_key_hints();
        std::vector<ui::EditorPopupItem> popup_items;
        std::string popup_title;
        std::optional<std::size_t> popup_selection;
        std::optional<std::string_view> popup_input;
        std::optional<std::size_t> popup_input_cursor;
        if (interaction != nullptr && interaction->request.kind == InteractionKind::Picker) {
            popup_title = interaction->request.prompt;
            popup_selection = interaction->candidates.empty()
                                  ? std::nullopt
                                  : std::optional<std::size_t>(interaction->selected);
            popup_items.reserve(interaction->candidates.size());
            popup_input = interaction->input.text();
            popup_input_cursor = interaction->input.caret();
            for (const InteractionCandidate& candidate : interaction->candidates) {
                popup_items.push_back({.label = candidate.label, .detail = candidate.detail});
            }
        } else if (!key_hints.empty()) {
            popup_title = command_loop_.pending_sequence_text() + " …";
            popup_items.reserve(key_hints.size());
            for (const KeyBindingHint& hint : key_hints) {
                const std::string_view detail = hint.command.empty() && hint.prefix
                                                    ? std::string_view("prefix")
                                                    : std::string_view(hint.command);
                popup_items.push_back({.label = hint.key, .detail = detail});
            }
        }
        if (application_.window_layout().leaves().size() > 1) {
            const WindowPartition partition =
                application_.window_layout().partition(size.rows - 1, size.cols);
            std::vector<ui::EditorPaneScene> panes;
            panes.reserve(partition.windows.size());
            for (const WindowPlacement& placement : partition.windows) {
                EditSession& pane_session = application_.session(placement.window);
                const DocumentSnapshot pane_snapshot = pane_session.snapshot();
                ViewportState& pane_state = pane_session.view().viewport();
                ui::EditorSceneViewState pane_view{
                    .viewport = {.top_line = pane_state.top_line,
                                 .top_line_offset = pane_state.top_line_offset,
                                 .left_column = pane_state.left_column},
                    .popup = {},
                };
                pane_view = ui::layout_editor_scene(
                    {.text = pane_snapshot.content(),
                     .caret = pane_session.caret(),
                     .rows = std::max(3, placement.rect.rows + 1),
                     .cols = std::max(1, placement.rect.columns),
                     .tab_width = pane_session.style().tab_width,
                     .reveal_caret = placement.window == application_.window_id(),
                     .popup_item_count = 0,
                     .popup_selection = std::nullopt},
                    pane_view);
                pane_state.top_line = pane_view.viewport.top_line;
                pane_state.top_line_offset = pane_view.viewport.top_line_offset;
                pane_state.left_column = pane_view.viewport.left_column;
                const ui::LineSigns pane_signs =
                    ui::line_signs(pane_session.buffer().save_point(), pane_snapshot.content());
                const bool active = placement.window == application_.window_id();
                ui::Scene pane_scene = ui::compose_editor_scene(
                    {.text = pane_snapshot.content(),
                     .tokens = application_.syntax_tokens(placement.window),
                     .signs = pane_signs,
                     .caret = pane_session.caret(),
                     .selection = pane_session.selection(),
                     .rows = std::max(3, placement.rect.rows + 1),
                     .cols = std::max(1, placement.rect.columns),
                     .tab_width = pane_session.style().tab_width,
                     .path = application_.path(placement.window),
                     .dirty = application_.dirty(placement.window),
                     .revision = pane_snapshot.revision(),
                     .style_origin = application_.style_origin(placement.window),
                     .last_key =
                         active ? std::string_view(application_.last_key()) : std::string_view(),
                     .pending_key = {},
                     .echo = {},
                     .echo_cursor_column = std::nullopt,
                     .echo_cursor_byte = std::nullopt,
                     .popup_title = {},
                     .popup_items = {},
                     .popup_selection = std::nullopt,
                     .popup_input = std::nullopt,
                     .popup_input_cursor = std::nullopt},
                    pane_view);
                panes.push_back({.id = std::format("window:{}:{}", placement.window.slot,
                                                   placement.window.generation),
                                 .rect = {.row = placement.rect.row,
                                          .col = placement.rect.column,
                                          .rows = placement.rect.rows,
                                          .cols = placement.rect.columns},
                                 .active = active,
                                 .scene = std::move(pane_scene)});
            }
            std::vector<ui::SceneDivider> dividers;
            dividers.reserve(partition.dividers.size());
            for (std::size_t index = 0; index < partition.dividers.size(); ++index) {
                const WindowDivider& divider = partition.dividers[index];
                dividers.push_back({.id = std::format("workspace/divider/{}", index),
                                    .axis = divider.axis == WindowSplitAxis::Rows
                                                ? ui::DividerAxis::Horizontal
                                                : ui::DividerAxis::Vertical,
                                    .position = divider.position,
                                    .start = divider.start,
                                    .length = divider.length});
            }

            ViewportState& active_state = session().view().viewport();
            ui::EditorSceneViewState chrome_view{
                .viewport = {.top_line = active_state.top_line,
                             .top_line_offset = active_state.top_line_offset,
                             .left_column = active_state.left_column},
                .popup = popup_viewport_,
            };
            chrome_view = ui::layout_editor_scene({.text = snap.content(),
                                                   .caret = session().caret(),
                                                   .rows = size.rows,
                                                   .cols = size.cols,
                                                   .tab_width = tab_width(),
                                                   .reveal_caret = false,
                                                   .popup_item_count = popup_items.size(),
                                                   .popup_selection = popup_selection},
                                                  chrome_view);
            popup_viewport_ = chrome_view.popup;
            ui::Scene chrome =
                ui::compose_editor_scene({.text = snap.content(),
                                          .tokens = tokens(),
                                          .signs = signs(),
                                          .caret = session().caret(),
                                          .selection = selection(),
                                          .rows = size.rows,
                                          .cols = size.cols,
                                          .tab_width = tab_width(),
                                          .path = path(),
                                          .dirty = dirty(),
                                          .revision = snap.revision(),
                                          .style_origin = application_.style_origin(),
                                          .last_key = application_.last_key(),
                                          .pending_key = {},
                                          .echo = echo_text,
                                          .echo_cursor_column = echo_cursor,
                                          .echo_cursor_byte = echo_cursor_byte,
                                          .popup_title = popup_title,
                                          .popup_items = popup_items,
                                          .popup_selection = popup_selection,
                                          .popup_input = popup_input,
                                          .popup_input_cursor = popup_input_cursor},
                                         chrome_view);
            return ui::compose_editor_workspace({.rows = size.rows, .cols = size.cols},
                                                std::move(panes), std::move(dividers),
                                                std::move(chrome));
        }
        ViewportState& state = session().view().viewport();
        ui::EditorSceneViewState view{
            .viewport = {.top_line = state.top_line,
                         .top_line_offset = state.top_line_offset,
                         .left_column = state.left_column},
            .popup = popup_viewport_,
        };
        view = ui::layout_editor_scene({.text = snap.content(),
                                        .caret = session().caret(),
                                        .rows = size.rows,
                                        .cols = size.cols,
                                        .tab_width = tab_width(),
                                        .reveal_caret = true,
                                        .popup_item_count = popup_items.size(),
                                        .popup_selection = popup_selection},
                                       view);
        state.top_line = view.viewport.top_line;
        state.top_line_offset = view.viewport.top_line_offset;
        state.left_column = view.viewport.left_column;
        popup_viewport_ = view.popup;
        ui::Scene scene = ui::compose_editor_scene({.text = snap.content(),
                                                    .tokens = tokens(),
                                                    .signs = signs(),
                                                    .caret = session().caret(),
                                                    .selection = selection(),
                                                    .rows = size.rows,
                                                    .cols = size.cols,
                                                    .tab_width = tab_width(),
                                                    .path = path(),
                                                    .dirty = dirty(),
                                                    .revision = snap.revision(),
                                                    .style_origin = application_.style_origin(),
                                                    .last_key = application_.last_key(),
                                                    .pending_key = {},
                                                    .echo = echo_text,
                                                    .echo_cursor_column = echo_cursor,
                                                    .echo_cursor_byte = echo_cursor_byte,
                                                    .popup_title = popup_title,
                                                    .popup_items = popup_items,
                                                    .popup_selection = popup_selection,
                                                    .popup_input = popup_input,
                                                    .popup_input_cursor = popup_input_cursor},
                                                   view);
        return scene;
    }

    void render() {
        term_.queue(ui::render_ansi(compose()));
        term_.flush();
    }

    Terminal term_;
    EventLoopWakeup wakeup_;
    EditorApplication application_;
    SearchCommands& search_commands_;
    CommandLoop& command_loop_;
    std::string& message_;
    ui::LineSigns signs_;
    BufferId signs_buffer_;
    RevisionId signs_rev_ = static_cast<RevisionId>(-1);
    std::uint32_t signs_gen_ = static_cast<std::uint32_t>(-1);
    ui::ListViewport popup_viewport_;
    bool command_keep_message_ = false;
    bool prompt_active_ = false;
    std::string prompt_label_;
    std::string prompt_input_;
};

} // namespace

int run_editor(const std::string& path, std::uint32_t initial_line) {
    try {
        Editor editor(path, std::nullopt, CppIndentStyle{}, "llvm (fallback)", initial_line);
        return editor.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "indent-core: %s\n", e.what());
        return 1;
    }
}

} // namespace cind::tui
