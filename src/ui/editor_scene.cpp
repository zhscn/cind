#include "ui/editor_scene.hpp"

#include "ui/char_width.hpp"
#include "ui/compose_line.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <format>

namespace cind::ui {

Scene compose_editor_scene(const EditorSceneInput& input, EditorViewport& viewport) {
    const int text_rows = std::max(1, input.rows - 2);
    const int text_column = text_area_column(input.text.line_count());
    const int text_width = std::max(1, input.cols - text_column);
    const LinePosition caret_position = input.text.position(input.caret);
    const int caret_column = display_column(input.text, input.caret, input.tab_width);

    if (input.reveal_caret) {
        if (caret_position.line < viewport.top_line) {
            viewport.top_line = caret_position.line;
        }
        if (caret_position.line >= viewport.top_line + static_cast<std::uint32_t>(text_rows)) {
            viewport.top_line = caret_position.line - static_cast<std::uint32_t>(text_rows) + 1;
        }
    }
    if (caret_column < viewport.left_column) {
        viewport.left_column = caret_column;
    }
    if (caret_column >= viewport.left_column + text_width) {
        viewport.left_column = caret_column - text_width + 1;
    }

    Scene scene;
    scene.rows = input.rows;
    scene.cols = input.cols;

    const int digits = gutter_digits(input.text.line_count());
    Region numbers{
        RegionRole::LineNumbers, {0, 0, text_rows, digits + 1}, {}, SurfaceClass::Gutter};
    Region marks{RegionRole::ChangeSigns, {0, digits + 1, text_rows, 1}, {}, SurfaceClass::Gutter};
    Region body{
        RegionRole::TextArea, {0, digits + 2, text_rows, text_width}, {}, SurfaceClass::Editor};
    Region status{RegionRole::StatusBar,
                  {text_rows, 0, 1, input.cols},
                  {},
                  SurfaceClass::Status,
                  VerticalAnchor::Bottom};
    Region echo{RegionRole::EchoArea,
                {text_rows + 1, 0, 1, input.cols},
                {},
                SurfaceClass::Echo,
                VerticalAnchor::Bottom};

    for (int row = 0; row < text_rows; ++row) {
        const std::uint32_t line = viewport.top_line + static_cast<std::uint32_t>(row);
        if (line >= input.text.line_count()) {
            body.prims.push_back({row, 0, "~", StyleClass::Gutter, false, PrimKind::Text,
                                  std::format("line:{}/empty", line)});
            continue;
        }
        numbers.prims.push_back({row, 0, std::format("{:>{}} ", line + 1, digits),
                                 StyleClass::Gutter, false, PrimKind::Text,
                                 std::format("line:{}/number", line)});
        switch (input.signs.at(line)) {
        case SignKind::Added:
            marks.prims.push_back({row, 0, "▎", StyleClass::SignAdded, false, PrimKind::ChangeBar,
                                   std::format("line:{}/sign", line)});
            break;
        case SignKind::Modified:
            marks.prims.push_back({row, 0, "▎", StyleClass::SignModified, false,
                                   PrimKind::ChangeBar, std::format("line:{}/sign", line)});
            break;
        case SignKind::DeletedAbove:
            marks.prims.push_back({row, 0, "▔", StyleClass::SignDeleted, false,
                                   PrimKind::ChangeDeletion, std::format("line:{}/sign", line)});
            break;
        case SignKind::None:
            break;
        }

        const TextRange content = input.text.line_content_range(line);
        const std::string bytes = input.text.substring(content);
        for (Run& run : build_line_runs({.text = bytes,
                                         .start_offset = content.start.value,
                                         .tab_width = input.tab_width,
                                         .left_col = viewport.left_column,
                                         .width = text_width,
                                         .selection = input.selection},
                                        input.tokens)) {
            const std::string id = std::format("line:{}/byte:{}", line, run.source_offset);
            body.prims.push_back(
                {row, run.col, std::move(run.text), run.style, run.selected, PrimKind::Text, id});
        }
    }

    std::string left =
        std::format(" {}{}  {}:{}  rev {}  style {} ", input.path, input.dirty ? " [+]" : "",
                    caret_position.line + 1, caret_column + 1, input.revision, input.style_origin);
    std::string key =
        input.last_key.empty() ? std::string() : std::format("key: {} ", input.last_key);
    key = std::string(clip_to_display_width(key, input.cols));
    const int key_width = display_width(key);
    left = std::string(clip_to_display_width(left, input.cols - key_width));
    const int fill = input.cols - display_width(left) - key_width;
    status.prims.push_back({0, 0, left + std::string(static_cast<std::size_t>(fill), ' '),
                            StyleClass::StatusBar, false, PrimKind::Text, "status:main"});
    if (!key.empty()) {
        status.prims.push_back({0, input.cols - key_width, key, StyleClass::StatusKey, false,
                                PrimKind::Text, "status:key"});
    }
    echo.prims.push_back(
        {0, 0, std::string(input.echo), StyleClass::Message, false, PrimKind::Text, "echo:main"});

    if (input.echo_cursor_column) {
        scene.cursor_row = input.rows;
        scene.cursor_col = *input.echo_cursor_column + 1;
    } else {
        const bool vertical_visible =
            caret_position.line >= viewport.top_line &&
            caret_position.line < viewport.top_line + static_cast<std::uint32_t>(text_rows);
        scene.cursor_visible = vertical_visible;
        scene.cursor_row =
            vertical_visible ? static_cast<int>(caret_position.line - viewport.top_line) + 1 : 1;
        scene.cursor_col = text_column + (caret_column - viewport.left_column) + 1;
    }

    scene.regions.push_back(std::move(numbers));
    scene.regions.push_back(std::move(marks));
    scene.regions.push_back(std::move(body));
    scene.regions.push_back(std::move(status));
    scene.regions.push_back(std::move(echo));
    return scene;
}

} // namespace cind::ui
