#pragma once

#include "cpp_lexer/token_buffer.hpp"
#include "document/text.hpp"
#include "ui/line_signs.hpp"
#include "ui/list_view.hpp"
#include "ui/scene.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cind::ui {

struct EditorViewport {
    std::uint32_t top_line = 0;
    float top_line_offset = 0.0F;
    int left_column = 0;
};

struct EditorPopupItem {
    std::string_view label;
    std::string_view detail;
};

struct EditorSceneInput {
    const Text& text;
    const TokenBuffer& tokens;
    const LineSigns& signs;
    TextOffset caret;
    std::optional<TextRange> selection;

    int rows = 24;
    int cols = 80;
    // Text viewport height in line-height units. Zero uses the scene's text
    // row count, which is appropriate for a terminal presenter.
    float visible_text_rows = 0.0F;
    int tab_width = 4;
    std::string_view path;
    bool dirty = false;
    RevisionId revision = 0;
    std::string_view style_origin;
    std::string_view last_key;
    std::string_view echo;
    bool reveal_caret = true;

    // A present value puts the caret on the echo line at this zero-based cell.
    std::optional<int> echo_cursor_column;

    std::string_view popup_title;
    std::span<const EditorPopupItem> popup_items;
    std::optional<std::size_t> popup_selection;
    // Present for an interactive picker. The GUI places this input inside
    // the popup; the TUI continues to expose it through the echo area.
    std::optional<std::string_view> popup_input;
    std::optional<std::size_t> popup_input_cursor;
};

// Composes the editor's standard five-region frame and scrolls `viewport` just
// enough to reveal the complete caret line. A fractional viewport position is
// represented as a negative grid offset, allowing either edge to contain a
// partial row. All geometry is in monospace line and cell units.
Scene compose_editor_scene(const EditorSceneInput& input, EditorViewport& viewport,
                           ListViewport& popup_viewport);

} // namespace cind::ui
