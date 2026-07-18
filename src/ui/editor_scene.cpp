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

std::size_t scene_popup_capacity(SceneGeometry geometry, std::size_t requested_capacity) {
    const int text_rows = scene_text_rows(geometry.rows);
    if (requested_capacity == 0 || geometry.cols < 16 || text_rows < 3) {
        return 0;
    }
    // The minibuffer reflows the frame instead of overlaying it: prompt and
    // candidates may claim at most half of the text rows.
    const int budget = std::max(1, text_rows / 2 - 1);
    return std::min<std::size_t>(
        requested_capacity,
        std::min<std::size_t>(editor_picker_capacity, static_cast<std::size_t>(budget)));
}

// Scene rows the minibuffer band occupies (prompt row + visible candidates);
// zero while no picker is active.
int scene_popup_rows(SceneGeometry geometry, std::size_t requested_capacity) {
    const std::size_t capacity = scene_popup_capacity(geometry, requested_capacity);
    return capacity == 0 ? 0 : static_cast<int>(capacity) + 1;
}

} // namespace

EditorSceneViewState layout_editor_scene(const EditorSceneLayoutInput& input,
                                         EditorSceneViewState current) {
    // The minibuffer band shrinks the text area, so caret reveal must use the
    // reduced height while a picker is active.
    const std::size_t popup_capacity =
        input.popup_capacity == 0 ? input.popup_item_count : input.popup_capacity;
    const int popup_rows =
        scene_popup_rows({.rows = input.rows, .cols = input.cols}, popup_capacity);
    const int text_rows = std::max(1, scene_text_rows(input.rows) - popup_rows);
    const float visible_rows_input =
        input.visible_text_rows > 0.0F
            ? std::max(1.0F, input.visible_text_rows - static_cast<float>(popup_rows))
            : 0.0F;
    const double visible_text_rows = scene_visible_text_rows(text_rows, visible_rows_input);
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
        scene_popup_capacity({.rows = input.rows, .cols = input.cols}, popup_capacity));
    return current;
}

Scene compose_editor_scene(const EditorSceneInput& input, const EditorSceneViewState& view) {
    const EditorViewport& viewport = view.viewport;
    // An active picker reflows the frame: document rows and the modeline move
    // up to give the minibuffer band its own rows above the echo line.
    const std::size_t popup_capacity =
        input.popup_capacity == 0 ? input.popup_items.size() : input.popup_capacity;
    const int popup_rows =
        scene_popup_rows({.rows = input.rows, .cols = input.cols}, popup_capacity);
    const int text_rows = std::max(1, scene_text_rows(input.rows) - popup_rows);
    const float visible_rows_input =
        input.visible_text_rows > 0.0F
            ? std::max(1.0F, input.visible_text_rows - static_cast<float>(popup_rows))
            : 0.0F;
    const double visible_text_rows = scene_visible_text_rows(text_rows, visible_rows_input);
    const int text_column = text_area_column(input.text.line_count());
    const int text_width = std::max(1, input.cols - text_column);
    const LinePosition caret_position = input.text.position(input.caret);
    const int caret_column = display_column(input.text, input.caret, input.tab_width);

    constexpr double offset_tolerance = 0.0001;

    Scene scene;
    scene.rows = input.rows;
    scene.cols = input.cols;
    scene.grid_offset_rows = -viewport.top_line_offset;
    scene.cursor_shape = input.cursor_shape;
    if (caret_position.line >= viewport.top_line) {
        const std::uint32_t row = caret_position.line - viewport.top_line;
        if (row < static_cast<std::uint32_t>(text_rows)) {
            scene.active_text_row = static_cast<int>(row);
        }
    }

    const int digits = gutter_digits(input.text.line_count());
    Region numbers{
        RegionRole::LineNumbers, {0, 0, text_rows, digits + 1}, {},
        SurfaceClass::Gutter,    VerticalAnchor::Grid,          "editor/gutter/line-numbers",
        input.revision};
    Region marks{
        RegionRole::ChangeSigns, {0, digits + 1, text_rows, 1}, {},
        SurfaceClass::Gutter,    VerticalAnchor::Grid,          "editor/gutter/change-signs",
        input.revision};
    Region body{RegionRole::TextArea,
                {0, digits + 2, text_rows, text_width},
                {},
                SurfaceClass::Editor,
                VerticalAnchor::Grid,
                "editor/document",
                input.revision};
    Region status{RegionRole::StatusBar, {text_rows, 0, 1, input.cols}, {},
                  SurfaceClass::Status,  VerticalAnchor::Bottom,        "editor/modeline",
                  input.revision};
    Region echo{RegionRole::EchoArea,
                {text_rows + popup_rows + 1, 0, 1, input.cols},
                {},
                SurfaceClass::Echo,
                VerticalAnchor::Bottom,
                "editor/echo",
                input.revision};
    numbers.set_document_mapping(
        {.first_line = viewport.top_line, .first_display_column = std::nullopt});
    marks.set_document_mapping(
        {.first_line = viewport.top_line, .first_display_column = std::nullopt});
    body.set_document_mapping(
        {.first_line = viewport.top_line, .first_display_column = viewport.left_column});

    for (int row = 0; row < text_rows; ++row) {
        const std::uint32_t line = viewport.top_line + static_cast<std::uint32_t>(row);
        if (line >= input.text.line_count()) {
            body.primitives().push_back({row, 0, "~", StyleClass::Gutter, false, PrimKind::Text,
                                         std::format("line:{}/empty", line)});
            continue;
        }
        numbers.primitives().push_back({row, 0, std::format("{:>{}} ", line + 1, digits),
                                        StyleClass::Gutter, false, PrimKind::Text,
                                        std::format("line:{}/number", line)});
        switch (input.signs.at(line)) {
        case SignKind::Added:
            marks.primitives().push_back({row, 0, "▎", StyleClass::SignAdded, false,
                                          PrimKind::ChangeBar, std::format("line:{}/sign", line)});
            break;
        case SignKind::Modified:
            marks.primitives().push_back({row, 0, "▎", StyleClass::SignModified, false,
                                          PrimKind::ChangeBar, std::format("line:{}/sign", line)});
            break;
        case SignKind::DeletedAbove:
            marks.primitives().push_back({row, 0, "▔", StyleClass::SignDeleted, false,
                                          PrimKind::ChangeDeletion,
                                          std::format("line:{}/sign", line)});
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
                                         .selections = input.selections},
                                        input.tokens)) {
            const std::string id = std::format("line:{}/byte:{}", line, run.source_offset);
            body.primitives().push_back(
                {row, run.col, std::move(run.text), run.style, run.selected, PrimKind::Text, id});
        }
    }

    for (std::size_t index = 0; index < input.position_hints.size(); ++index) {
        const PositionHint& hint = input.position_hints[index];
        if (hint.position.value > input.text.size_bytes()) {
            continue;
        }
        const LinePosition position = input.text.position(hint.position);
        if (position.line < viewport.top_line) {
            continue;
        }
        const std::uint32_t visible_row = position.line - viewport.top_line;
        if (visible_row >= static_cast<std::uint32_t>(text_rows)) {
            continue;
        }
        const int column =
            display_column(input.text, hint.position, input.tab_width) - viewport.left_column;
        if (column < 0 || column >= text_width) {
            continue;
        }
        int source_width = 1;
        const TextRange line_content = input.text.line_content_range(position.line);
        if (hint.position < line_content.end) {
            const std::string tail = input.text.substring({hint.position, line_content.end});
            if (!tail.empty()) {
                source_width = std::max(1, decode_grapheme(tail).width);
            }
        }
        const int span_cols = std::max(source_width, std::max(1, display_width(hint.label)));
        body.primitives().push_back({static_cast<int>(visible_row), column, hint.label,
                                     StyleClass::PositionHint, false, PrimKind::PositionHint,
                                     std::format("hint:{}/byte:{}", index, hint.position.value),
                                     span_cols});
    }

    status.set_status({.path = std::string(input.path),
                       .dirty = input.dirty,
                       .line = caret_position.line + 1,
                       .column = static_cast<std::uint32_t>(caret_column + 1),
                       .line_count = input.text.line_count(),
                       .revision = input.revision,
                       .style_origin = std::string(input.style_origin),
                       .key = std::string(input.last_key),
                       .input_state = std::string(input.input_state_indicator)});
    echo.set_echo({
        .text = std::string(input.echo),
        .cursor_byte = input.echo_cursor_byte
                           ? std::optional(std::min(*input.echo_cursor_byte, input.echo.size()))
                           : std::nullopt,
        .key = std::string(input.pending_key),
    });

    std::optional<Region> popup;
    const std::optional<std::size_t> popup_selection =
        input.popup_selection && *input.popup_selection < input.popup_items.size()
            ? input.popup_selection
            : std::nullopt;
    if (popup_rows > 0) {
        const std::size_t capacity = static_cast<std::size_t>(popup_rows - 1);
        const std::size_t visible_count = std::min(capacity, input.popup_items.size());
        const std::size_t maximum_first = input.popup_items.size() - visible_count;
        const std::size_t first = std::min(view.popup.first_item(), maximum_first);
        // The minibuffer band claims the reflowed rows between the modeline
        // and the echo line, always full width.
        popup.emplace(RegionRole::Popup, Rect{text_rows + 1, 0, popup_rows, input.cols},
                      std::vector<Prim>{}, SurfaceClass::Status, VerticalAnchor::Bottom,
                      "editor/minibuffer", input.revision);
        popup->set_popup({});
        Region::PopupContent& popup_content = *popup->popup();
        popup_content.title = input.popup_title;
        if (input.popup_input) {
            popup_content.input = std::string(*input.popup_input);
            popup_content.input_cursor =
                std::min(input.popup_input_cursor.value_or(input.popup_input->size()),
                         input.popup_input->size());
        }
        popup_content.first_item = first;
        popup_content.total_items = input.popup_items.size();
        popup_content.selected_item = popup_selection;
        popup_content.items.reserve(visible_count);

        for (std::size_t offset = 0; offset < visible_count; ++offset) {
            const std::size_t index = first + offset;
            const EditorPopupItem& item = input.popup_items[index];
            popup_content.items.push_back(
                {.label = std::string(item.label), .detail = std::string(item.detail)});
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

Scene compose_editor_workspace(EditorWorkspaceGeometry geometry, std::vector<EditorPaneScene> panes,
                               std::vector<SceneDivider> dividers, Scene chrome) {
    Scene workspace;
    workspace.rows = geometry.rows;
    workspace.cols = geometry.cols;
    workspace.dividers = std::move(dividers);
    workspace.cursor_visible = false;

    workspace.panes.reserve(panes.size());
    for (const EditorPaneScene& pane : panes) {
        workspace.panes.push_back({.id = pane.id, .rect = pane.rect, .active = pane.active});
    }

    // The active document is first in region order so existing single-caret
    // presenter fast paths continue to select the focused shaped layout.
    std::ranges::stable_sort(panes, [](const EditorPaneScene& left, const EditorPaneScene& right) {
        return left.active && !right.active;
    });
    for (EditorPaneScene& pane : panes) {
        if (pane.rect.rows <= 0 || pane.rect.cols <= 0) {
            continue;
        }
        const int text_rows = std::max(1, pane.rect.rows - 1);
        for (Region& region : pane.scene.regions) {
            if (region.role == RegionRole::EchoArea || region.role == RegionRole::Popup) {
                continue;
            }
            region.rect.row += pane.rect.row;
            region.rect.col += pane.rect.col;
            region.rect.rows = std::min(region.rect.rows, text_rows);
            region.rect.cols = std::min(region.rect.cols, pane.rect.cols);
            region.id = std::format("workspace/{}/{}", pane.id, region.id);
            region.pane_id = pane.id;
            region.active = pane.active;
            if (region.role == RegionRole::StatusBar) {
                region.rect.row = pane.rect.row + pane.rect.rows - 1;
                region.rect.rows = 1;
                region.rect.col = pane.rect.col;
                region.rect.cols = pane.rect.cols;
                region.vertical_anchor = VerticalAnchor::Cell;
            } else {
                region.vertical_anchor = VerticalAnchor::PaneGrid;
                region.content_offset_rows = pane.scene.grid_offset_rows;
            }
            workspace.regions.push_back(std::move(region));
        }
        if (pane.active) {
            if (pane.scene.active_text_row) {
                workspace.active_text_row = pane.rect.row + pane.scene.active_text_row.value_or(0);
            }
            workspace.cursor_row = pane.rect.row + pane.scene.cursor_row;
            workspace.cursor_col = pane.rect.col + pane.scene.cursor_col;
            workspace.cursor_visible = pane.scene.cursor_visible;
            workspace.cursor_shape = pane.scene.cursor_shape;
        }
    }

    const bool chrome_owns_cursor = std::ranges::any_of(chrome.regions, [](const Region& region) {
        return (region.role == RegionRole::Popup && region.popup() != nullptr &&
                region.popup()->input.has_value()) ||
               (region.role == RegionRole::EchoArea && region.echo() != nullptr &&
                region.echo()->cursor_byte.has_value());
    });
    for (Region& region : chrome.regions) {
        if (region.role != RegionRole::EchoArea && region.role != RegionRole::Popup) {
            continue;
        }
        workspace.regions.push_back(std::move(region));
    }
    if (chrome_owns_cursor) {
        workspace.cursor_row = chrome.cursor_row;
        workspace.cursor_col = chrome.cursor_col;
        workspace.cursor_visible = chrome.cursor_visible;
        workspace.cursor_shape = chrome.cursor_shape;
    }
    return workspace;
}

} // namespace cind::ui
