#pragma once

#include "cpp_lexer/token_buffer.hpp"
#include "document/text.hpp"
#include "ui/line_signs.hpp"
#include "ui/scene.hpp"

#include <optional>
#include <string_view>

namespace cind::ui {

struct EditorViewport {
    std::uint32_t top_line = 0;
    int left_column = 0;
};

struct EditorSceneInput {
    const Text& text;
    const TokenBuffer& tokens;
    const LineSigns& signs;
    TextOffset caret;
    std::optional<TextRange> selection;

    int rows = 24;
    int cols = 80;
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
};

// Composes the editor's standard five-region frame and scrolls `viewport`
// just enough to keep the caret visible. All geometry is in monospace cells;
// presenters decide how those cells map to terminal positions or pixels.
Scene compose_editor_scene(const EditorSceneInput& input, EditorViewport& viewport);

} // namespace cind::ui
