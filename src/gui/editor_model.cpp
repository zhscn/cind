#include "gui/editor_model.hpp"

#include "document/text.hpp"
#include "ui/char_width.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace cind::gui {

EditorModel::EditorModel(std::string path, std::string initial, CppIndentStyle style,
                         std::string style_origin, std::uint32_t initial_line)
    : application_({.path = std::move(path),
                    .initial_text = std::move(initial),
                    .style = style,
                    .style_origin = std::move(style_origin),
                    .initial_line = initial_line}) {
    application_.set_message("SDL3 Wayland · Skia · C-s save · C-q quit");
}

ui::Scene EditorModel::compose(int rows, int columns, float visible_text_rows) {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    std::string minibuffer_echo;
    std::optional<int> echo_cursor;
    const std::string_view echo = [&]() -> std::string_view {
        if (const MinibufferState* minibuffer = application_.command_loop().minibuffer()) {
            minibuffer_echo = minibuffer->request.prompt + minibuffer->input;
            echo_cursor = ui::display_width(minibuffer_echo);
            return minibuffer_echo;
        }
        return preedit_.empty() ? std::string_view(application_.message())
                                : std::string_view(preedit_);
    }();
    ViewportState& state = session.view().viewport();
    ui::EditorViewport viewport{.top_line = state.top_line,
                                .top_line_offset = state.top_line_offset,
                                .left_column = state.left_column};
    ui::Scene scene = ui::compose_editor_scene({.text = snapshot.content(),
                                                .tokens = session.analysis().tree.tokens(),
                                                .signs = signs(),
                                                .caret = session.caret(),
                                                .selection = std::nullopt,
                                                .rows = rows,
                                                .cols = columns,
                                                .visible_text_rows = visible_text_rows,
                                                .tab_width = session.style().tab_width,
                                                .path = application_.path(),
                                                .dirty = application_.dirty(),
                                                .revision = snapshot.revision(),
                                                .style_origin = application_.style_origin(),
                                                .last_key = application_.last_key(),
                                                .echo = echo,
                                                .reveal_caret = application_.reveal_caret(),
                                                .echo_cursor_column = echo_cursor},
                                               viewport);
    state.top_line = viewport.top_line;
    state.top_line_offset = viewport.top_line_offset;
    state.left_column = viewport.left_column;
    return scene;
}

bool EditorModel::handle_key(KeyStroke key, int page_rows) {
    return application_.handle_key(key, page_rows);
}

void EditorModel::insert_text(std::string_view text) {
    application_.insert_text(text);
    preedit_.clear();
}

void EditorModel::set_preedit(std::string_view text) {
    preedit_ = text.empty() ? std::string() : std::format("IME · {}", text);
}

void EditorModel::click(ui::CellPoint point) {
    application_.reset_preferred_column();
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const Text& text = snapshot.content();
    const int text_column = ui::text_area_column(text.line_count());
    const int visible_rows = std::max(1, last_rows_ - 2);
    if (point.row < 0 || point.row >= visible_rows) {
        return;
    }
    const std::uint32_t line =
        std::min(session.view().viewport().top_line + static_cast<std::uint32_t>(point.row),
                 text.line_count() - 1);
    if (point.column < text_column) {
        session.set_caret(text.line_start(line));
        application_.show_caret();
        return;
    }
    const int display_column =
        session.view().viewport().left_column + std::max(0, point.column - text_column);
    session.set_caret(ui::offset_at_display_column(text, {.line = line, .column = display_column},
                                                   session.style().tab_width));
    application_.show_caret();
}

void EditorModel::scroll_lines(int delta) {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const int last_line = static_cast<int>(snapshot.content().line_count()) - 1;
    ViewportState& viewport = session.view().viewport();
    const double position = static_cast<double>(viewport.top_line) +
                            static_cast<double>(viewport.top_line_offset) +
                            static_cast<double>(delta);
    const double clamped = std::clamp(position, 0.0, static_cast<double>(last_line));
    const double integral = std::floor(clamped);
    viewport.top_line = static_cast<std::uint32_t>(integral);
    viewport.top_line_offset = static_cast<float>(clamped - integral);
    application_.hide_caret();
}

EditorStateSnapshot EditorModel::inspect() {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = session.caret();
    const ViewportState& view = session.view().viewport();
    const CommandLoop& command_loop = application_.command_loop();
    const EditorRuntime& runtime = application_.runtime();
    CommandLoopStateSnapshot command_state{.keymaps = {},
                                           .pending_keys = command_loop.pending_sequence_text(),
                                           .pending_keymap = {},
                                           .repeat_count = command_loop.repeat_count(),
                                           .last_command = application_.last_command(),
                                           .minibuffer = {}};
    for (const KeymapId keymap : command_loop.keymaps()) {
        command_state.keymaps.push_back(runtime.keymaps().definition(keymap).name);
    }
    if (const std::optional<KeymapId> keymap = command_loop.pending_keymap()) {
        command_state.pending_keymap = runtime.keymaps().definition(*keymap).name;
    }
    if (const MinibufferState* minibuffer = command_loop.minibuffer()) {
        command_state.minibuffer = {.active = true,
                                    .prompt = minibuffer->request.prompt,
                                    .input = minibuffer->input,
                                    .history = minibuffer->request.history,
                                    .completion_provider = minibuffer->request.completion_provider};
    }
    return {.path = application_.path(),
            .revision = snapshot.revision(),
            .document_bytes = text.size_bytes(),
            .line_count = text.line_count(),
            .dirty = application_.dirty(),
            .caret = caret,
            .caret_position = text.position(caret),
            .caret_display_column = ui::display_column(text, caret, session.style().tab_width),
            .viewport = {.top_line = view.top_line,
                         .top_line_offset = view.top_line_offset,
                         .left_column = view.left_column},
            .line_signs = signs(),
            .tab_width = session.style().tab_width,
            .style_origin = application_.style_origin(),
            .message = application_.message(),
            .preedit = preedit_,
            .last_key = application_.last_key(),
            .command_loop = std::move(command_state),
            .quit_armed = application_.quit_armed(),
            .quit = application_.should_quit()};
}

const ui::LineSigns& EditorModel::signs() {
    const DocumentSnapshot snapshot = application_.session().snapshot();
    if (sign_revision_ != snapshot.revision() ||
        sign_generation_ != application_.save_generation()) {
        signs_ = ui::line_signs(application_.session().buffer().save_point(), snapshot.content());
        sign_revision_ = snapshot.revision();
        sign_generation_ = application_.save_generation();
    }
    return signs_;
}

} // namespace cind::gui
