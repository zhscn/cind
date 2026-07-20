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
    Editor(const std::string& path, std::optional<std::string> initial, std::uint32_t initial_line)
        : term_(), wakeup_(),
          application_({.path = path,
                        .initial_text = std::move(initial),
                        .initial_line = initial_line,
                        .platform_services = {.write_clipboard = [this](std::string_view text)
                                                  -> std::expected<void, std::string> {
                                                  term_.set_clipboard_text(text);
                                                  return {};
                                              },
                                              .read_clipboard = {},
                                              .wake_event_loop = [this] { wakeup_.notify(); }},
                        .init_file = discover_user_init_file()}) {}

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
        if (key.kind == KeyKind::Char) {
            if (!key.text.empty()) {
                char32_t character = ui::decode_utf8(key.text).cp;
                KeyModifiers modifiers;
                if (character >= U'A' && character <= U'Z') {
                    character = U'a' + (character - U'A');
                    modifiers |= KeyModifier::Shift;
                }
                const bool handled = application_.handle_key(
                    KeyStroke::character_key(character, modifiers), text_rows());
                if (!handled) {
                    application_.insert_text(key.text);
                }
            }
        } else if (key.kind == KeyKind::Eof) {
            (void)application_.request_close(true);
        } else if (const std::optional<KeyStroke> stroke = normalize_key(key)) {
            (void)application_.handle_key(*stroke, text_rows());
        }
    }

    std::optional<TextRange> selection() const { return session().selection(); }

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
        const ChromeContent chrome_content = application_.chrome_content();
        application_.resolve_completion_window(completion_viewport_.first_item());
        std::vector<ChromeItem> completion_items;
        const CompletionState* completion = application_.completion().state();
        if (completion != nullptr) {
            completion_items.reserve(completion->matches.size());
            for (const CompletionMatch& match : completion->matches) {
                completion_items.push_back({.label = match.item.label,
                                            .detail = match.item.detail,
                                            .kind = match.item.kind});
            }
        }
        const std::optional<std::size_t> completion_selection = application_.completion_selection();
        const std::optional<TextOffset> completion_anchor =
            completion != nullptr ? std::optional(completion->request.anchor) : std::nullopt;
        const std::optional<std::string_view> completion_documentation =
            completion != nullptr && completion_selection &&
                    *completion_selection < completion->matches.size() &&
                    !completion->matches[*completion_selection].item.documentation.empty()
                ? std::optional<std::string_view>(
                      completion->matches[*completion_selection].item.documentation)
                : std::nullopt;
        const std::optional<std::string_view> popup_input =
            chrome_content.popup_input
                ? std::optional<std::string_view>(*chrome_content.popup_input)
                : std::nullopt;
        const InputStateRegistry::Definition& active_input_state = application_.input_state();
        const std::vector<TextRange> active_selections = session().selected_ranges();
        PositionHintProviderResult active_hint_result =
            application_.position_hints(application_.window_id());
        const std::vector<PositionHint> active_position_hints =
            active_hint_result ? std::move(*active_hint_result) : std::vector<PositionHint>{};
        const ModelineContent active_modeline = application_.modeline(application_.window_id());
        if (application_.window_layout().leaves().size() > 1) {
            const WindowPartition partition =
                application_.window_layout().partition(size.rows - 1, size.cols);
            std::vector<ui::EditorPaneScene> panes;
            panes.reserve(partition.windows.size());
            for (const WindowPlacement& placement : partition.windows) {
                EditSession& pane_session = application_.session(placement.window);
                const DocumentSnapshot pane_snapshot = pane_session.snapshot();
                ViewportState& pane_state = pane_session.view().viewport();
                const bool active = placement.window == application_.window_id();
                ui::EditorSceneViewState pane_view{
                    .viewport = {.top_line = pane_state.top_line,
                                 .top_line_offset = pane_state.top_line_offset,
                                 .left_column = pane_state.left_column},
                    .popup = {},
                    .completion = active ? completion_viewport_ : ui::ListViewport{},
                };
                pane_view = ui::layout_editor_scene(
                    {.text = pane_snapshot.content(),
                     .caret = pane_session.caret(),
                     .rows = std::max(3, placement.rect.rows + 1),
                     .cols = std::max(1, placement.rect.columns),
                     .tab_width = pane_session.style().tab_width,
                     .reveal_caret = placement.window == application_.window_id(),
                     .popup_item_count = 0,
                     .popup_selection = std::nullopt,
                     .completion_item_count = active ? completion_items.size() : 0,
                     .completion_selection = active ? completion_selection : std::nullopt},
                    pane_view);
                pane_state.top_line = pane_view.viewport.top_line;
                pane_state.top_line_offset = pane_view.viewport.top_line_offset;
                pane_state.left_column = pane_view.viewport.left_column;
                if (active) {
                    completion_viewport_ = pane_view.completion;
                }
                const ui::LineSigns pane_signs =
                    ui::line_signs(pane_session.buffer().save_point(), pane_snapshot.content());
                const InputStateRegistry::Definition& pane_input_state =
                    application_.input_state(placement.window);
                const std::vector<TextRange> pane_selections = pane_session.selected_ranges();
                PositionHintProviderResult pane_hint_result =
                    application_.position_hints(placement.window);
                const std::vector<PositionHint> pane_position_hints =
                    pane_hint_result ? std::move(*pane_hint_result) : std::vector<PositionHint>{};
                const ModelineContent pane_modeline = application_.modeline(placement.window);
                ui::Scene pane_scene = ui::compose_editor_scene(
                    {.text = pane_snapshot.content(),
                     .tokens = application_.syntax_tokens(placement.window),
                     .signs = pane_signs,
                     .caret = pane_session.caret(),
                     .selections = pane_selections,
                     .position_hints = pane_position_hints,
                     .rows = std::max(3, placement.rect.rows + 1),
                     .cols = std::max(1, placement.rect.columns),
                     .tab_width = pane_session.style().tab_width,
                     .revision = pane_snapshot.revision(),
                     .modeline = pane_modeline,
                     .cursor_shape = pane_input_state.cursor,
                     .pending_key = {},
                     .echo = {},
                     .echo_cursor_column = std::nullopt,
                     .echo_cursor_byte = std::nullopt,
                     .popup_title = {},
                     .popup_items = {},
                     .popup_selection = std::nullopt,
                     .popup_input = std::nullopt,
                     .popup_input_cursor = std::nullopt,
                     .completion_items = active ? std::span<const ChromeItem>(completion_items)
                                                : std::span<const ChromeItem>{},
                     .completion_selection = active ? completion_selection : std::nullopt,
                     .completion_anchor = active ? completion_anchor : std::nullopt,
                     .completion_documentation = active ? completion_documentation : std::nullopt},
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
                .completion = completion_viewport_,
            };
            chrome_view =
                ui::layout_editor_scene({.text = snap.content(),
                                         .caret = session().caret(),
                                         .rows = size.rows,
                                         .cols = size.cols,
                                         .tab_width = tab_width(),
                                         .reveal_caret = false,
                                         .popup_item_count = chrome_content.popup_items.size(),
                                         .popup_capacity = chrome_content.popup_capacity,
                                         .popup_selection = chrome_content.popup_selection,
                                         .completion_item_count = completion_items.size(),
                                         .completion_selection = completion_selection},
                                        chrome_view);
            popup_viewport_ = chrome_view.popup;
            completion_viewport_ = chrome_view.completion;
            ui::Scene chrome =
                ui::compose_editor_scene({.text = snap.content(),
                                          .tokens = tokens(),
                                          .signs = signs(),
                                          .caret = session().caret(),
                                          .selections = active_selections,
                                          .position_hints = active_position_hints,
                                          .rows = size.rows,
                                          .cols = size.cols,
                                          .tab_width = tab_width(),
                                          .revision = snap.revision(),
                                          .modeline = active_modeline,
                                          .cursor_shape = active_input_state.cursor,
                                          .pending_key = chrome_content.pending_key,
                                          .echo = chrome_content.echo,
                                          .echo_cursor_column = chrome_content.echo_cursor_column,
                                          .echo_cursor_byte = chrome_content.echo_cursor_byte,
                                          .popup_title = chrome_content.popup_title,
                                          .popup_items = chrome_content.popup_items,
                                          .popup_capacity = chrome_content.popup_capacity,
                                          .popup_selection = chrome_content.popup_selection,
                                          .popup_input = popup_input,
                                          .popup_input_cursor = chrome_content.popup_input_cursor,
                                          .completion_items = {},
                                          .completion_selection = std::nullopt,
                                          .completion_anchor = std::nullopt,
                                          .completion_documentation = std::nullopt},
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
            .completion = completion_viewport_,
        };
        view = ui::layout_editor_scene({.text = snap.content(),
                                        .caret = session().caret(),
                                        .rows = size.rows,
                                        .cols = size.cols,
                                        .tab_width = tab_width(),
                                        .reveal_caret = true,
                                        .popup_item_count = chrome_content.popup_items.size(),
                                        .popup_capacity = chrome_content.popup_capacity,
                                        .popup_selection = chrome_content.popup_selection,
                                        .completion_item_count = completion_items.size(),
                                        .completion_selection = completion_selection},
                                       view);
        state.top_line = view.viewport.top_line;
        state.top_line_offset = view.viewport.top_line_offset;
        state.left_column = view.viewport.left_column;
        popup_viewport_ = view.popup;
        completion_viewport_ = view.completion;
        ui::Scene scene =
            ui::compose_editor_scene({.text = snap.content(),
                                      .tokens = tokens(),
                                      .signs = signs(),
                                      .caret = session().caret(),
                                      .selections = active_selections,
                                      .position_hints = active_position_hints,
                                      .rows = size.rows,
                                      .cols = size.cols,
                                      .tab_width = tab_width(),
                                      .revision = snap.revision(),
                                      .modeline = active_modeline,
                                      .cursor_shape = active_input_state.cursor,
                                      .pending_key = chrome_content.pending_key,
                                      .echo = chrome_content.echo,
                                      .echo_cursor_column = chrome_content.echo_cursor_column,
                                      .echo_cursor_byte = chrome_content.echo_cursor_byte,
                                      .popup_title = chrome_content.popup_title,
                                      .popup_items = chrome_content.popup_items,
                                      .popup_capacity = chrome_content.popup_capacity,
                                      .popup_selection = chrome_content.popup_selection,
                                      .popup_input = popup_input,
                                      .popup_input_cursor = chrome_content.popup_input_cursor,
                                      .completion_items = completion_items,
                                      .completion_selection = completion_selection,
                                      .completion_anchor = completion_anchor,
                                      .completion_documentation = completion_documentation},
                                     view);
        return scene;
    }

    void render() {
        term_.queue(ui::render_ansi(compose(), application_.presentation_theme(),
                                    application_.presentation_styles()));
        term_.flush();
    }

    Terminal term_;
    EventLoopWakeup wakeup_;
    EditorApplication application_;
    ui::LineSigns signs_;
    BufferId signs_buffer_;
    RevisionId signs_rev_ = static_cast<RevisionId>(-1);
    std::uint32_t signs_gen_ = static_cast<std::uint32_t>(-1);
    ui::ListViewport popup_viewport_;
    ui::ListViewport completion_viewport_;
};

} // namespace

int run_editor(const std::string& path, std::uint32_t initial_line) {
    try {
        Editor editor(path, std::nullopt, initial_line);
        return editor.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "indent-core: %s\n", e.what());
        return 1;
    }
}

} // namespace cind::tui
