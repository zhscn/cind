#include "ui/editor_scene.hpp"

#include "ui/char_width.hpp"
#include "ui/compose_line.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace cind::ui {

namespace {

int scene_text_rows(int rows) {
    return std::max(1, rows - 2);
}

struct SceneGeometry {
    int rows = 0;
    int cols = 0;
};

double scene_visible_text_rows(int text_rows, float visible_text_rows) {
    return std::clamp(visible_text_rows > 0.0F ? static_cast<double>(visible_text_rows)
                                               : static_cast<double>(text_rows),
                      1.0, static_cast<double>(text_rows));
}

std::size_t scene_popup_capacity(SceneGeometry geometry, std::size_t item_count) {
    constexpr std::size_t maximum_popup_items = 12;
    const int text_rows = scene_text_rows(geometry.rows);
    if (item_count == 0 || geometry.cols < 16 || text_rows < 3) {
        return 0;
    }
    return std::min<std::size_t>(
        item_count,
        std::min<std::size_t>(maximum_popup_items, static_cast<std::size_t>(text_rows - 1)));
}

} // namespace

EditorSceneViewState layout_editor_scene(const EditorSceneLayoutInput& input,
                                         EditorSceneViewState current) {
    const int text_rows = scene_text_rows(input.rows);
    const double visible_text_rows = scene_visible_text_rows(text_rows, input.visible_text_rows);
    const int text_column = text_area_column(input.text.line_count());
    const int text_width = std::max(1, input.cols - text_column);
    const LinePosition caret_position = input.text.position(input.caret);
    const int caret_column = display_column(input.text, input.caret, input.tab_width);

    double scroll_top = static_cast<double>(current.viewport.top_line) +
                        std::clamp(static_cast<double>(current.viewport.top_line_offset), 0.0,
                                   std::nextafter(1.0, 0.0));
    if (input.reveal_caret) {
        const double caret_top = static_cast<double>(caret_position.line);
        const double caret_bottom = caret_top + 1.0;
        if (caret_top < scroll_top) {
            scroll_top = caret_top;
        } else if (caret_bottom > scroll_top + visible_text_rows) {
            scroll_top = caret_bottom - visible_text_rows;
        }
    }
    scroll_top = std::max(0.0, scroll_top);
    double integral_scroll = std::floor(scroll_top);
    double line_offset = scroll_top - integral_scroll;
    constexpr double offset_tolerance = 0.0001;
    if (line_offset < offset_tolerance) {
        line_offset = 0.0;
    } else if (line_offset > 1.0 - offset_tolerance) {
        integral_scroll += 1.0;
        line_offset = 0.0;
    }
    const double maximum_line = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    current.viewport.top_line = static_cast<std::uint32_t>(std::min(integral_scroll, maximum_line));
    current.viewport.top_line_offset = static_cast<float>(line_offset);

    if (caret_column < current.viewport.left_column) {
        current.viewport.left_column = caret_column;
    }
    if (caret_column >= current.viewport.left_column + text_width) {
        current.viewport.left_column = caret_column - text_width + 1;
    }

    current.popup.reveal(
        input.popup_selection, input.popup_item_count,
        scene_popup_capacity({.rows = input.rows, .cols = input.cols}, input.popup_item_count));
    return current;
}

Scene compose_editor_scene(const EditorSceneInput& input, const EditorSceneViewState& view) {
    const EditorViewport& viewport = view.viewport;
    const int text_rows = scene_text_rows(input.rows);
    const double visible_text_rows = scene_visible_text_rows(text_rows, input.visible_text_rows);
    const int text_column = text_area_column(input.text.line_count());
    const int text_width = std::max(1, input.cols - text_column);
    const LinePosition caret_position = input.text.position(input.caret);
    const int caret_column = display_column(input.text, input.caret, input.tab_width);

    constexpr double offset_tolerance = 0.0001;

    Scene scene;
    scene.rows = input.rows;
    scene.cols = input.cols;
    scene.grid_offset_rows = -viewport.top_line_offset;
    if (caret_position.line >= viewport.top_line) {
        const std::uint32_t row = caret_position.line - viewport.top_line;
        if (row < static_cast<std::uint32_t>(text_rows)) {
            scene.active_text_row = static_cast<int>(row);
        }
    }

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

    status.status.emplace();
    status.status->path = std::string(input.path);
    status.status->dirty = input.dirty;
    status.status->line = caret_position.line + 1;
    status.status->column = static_cast<std::uint32_t>(caret_column + 1);
    status.status->line_count = input.text.line_count();
    status.status->revision = input.revision;
    status.status->style_origin = std::string(input.style_origin);
    status.status->key = std::string(input.last_key);

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
    echo.echo = Region::EchoContent{
        .text = std::string(input.echo),
        .cursor_byte = input.echo_cursor_byte
                           ? std::optional(std::min(*input.echo_cursor_byte, input.echo.size()))
                           : std::nullopt,
    };

    std::optional<Region> popup;
    const std::optional<std::size_t> popup_selection =
        input.popup_selection && *input.popup_selection < input.popup_items.size()
            ? input.popup_selection
            : std::nullopt;
    if (!input.popup_items.empty() && input.cols >= 16 && text_rows >= 3) {
        const std::size_t capacity = scene_popup_capacity({.rows = input.rows, .cols = input.cols},
                                                          input.popup_items.size());
        const std::size_t visible_count = std::min(input.popup_items.size(), capacity);
        const std::size_t maximum_first = input.popup_items.size() - visible_count;
        const std::size_t first = std::min(view.popup.first_item(), maximum_first);
        const int popup_width = std::min(input.cols - 4, 88);
        const int popup_rows = static_cast<int>(visible_count) + 1;
        const int popup_row = std::max(0, text_rows - popup_rows);
        const int popup_col = std::max(0, (input.cols - popup_width) / 2);
        popup.emplace(RegionRole::Popup, Rect{popup_row, popup_col, popup_rows, popup_width},
                      std::vector<Prim>{}, SurfaceClass::Status, VerticalAnchor::Overlay);
        popup->popup.emplace();
        popup->popup->title = input.popup_title;
        if (input.popup_input) {
            popup->popup->input = std::string(*input.popup_input);
            popup->popup->input_cursor =
                std::min(input.popup_input_cursor.value_or(input.popup_input->size()),
                         input.popup_input->size());
        }
        popup->popup->first_item = first;
        popup->popup->total_items = input.popup_items.size();
        popup->popup->selected_item = popup_selection;
        popup->popup->items.reserve(visible_count);

        std::string title = std::string(clip_to_display_width(input.popup_title, popup_width));
        title.append(static_cast<std::size_t>(popup_width - display_width(title)), ' ');
        popup->prims.push_back(
            {0, 0, std::move(title), StyleClass::StatusKey, false, PrimKind::Text, "popup:title"});
        for (std::size_t offset = 0; offset < visible_count; ++offset) {
            const std::size_t index = first + offset;
            const EditorPopupItem& item = input.popup_items[index];
            popup->popup->items.push_back(
                {.label = std::string(item.label), .detail = std::string(item.detail)});
            std::string row = item.detail.empty()
                                  ? std::string(item.label)
                                  : std::format("{:<10} {}", item.label, item.detail);
            row = std::string(clip_to_display_width(row, popup_width));
            row.append(static_cast<std::size_t>(popup_width - display_width(row)), ' ');
            popup->prims.push_back({static_cast<int>(offset) + 1, 0, std::move(row),
                                    StyleClass::Popup, popup_selection == index, PrimKind::Text,
                                    std::format("popup:item:{}", index)});
        }
    }

    if (input.echo_cursor_column) {
        scene.cursor_row = input.rows;
        scene.cursor_col = *input.echo_cursor_column + 1;
    } else {
        const double caret_top = static_cast<double>(caret_position.line);
        const double caret_bottom = caret_top + 1.0;
        const double scroll_top =
            static_cast<double>(viewport.top_line) + static_cast<double>(viewport.top_line_offset);
        const bool vertical_visible =
            caret_top + offset_tolerance >= scroll_top &&
            caret_bottom <= scroll_top + visible_text_rows + offset_tolerance;
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
    if (popup) {
        scene.regions.push_back(std::move(*popup));
    }
    return scene;
}

} // namespace cind::ui
