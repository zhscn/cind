#pragma once

#include "cpp_lexer/token_buffer.hpp"
#include "document/text.hpp"
#include "presentation/position_hint.hpp"
#include "ui/line_signs.hpp"
#include "ui/list_view.hpp"
#include "ui/scene.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cind::ui {

inline constexpr std::size_t editor_picker_capacity = 12;

struct EditorViewport {
    std::uint32_t top_line = 0;
    float top_line_offset = 0.0F;
    int left_column = 0;
};

struct EditorPopupItem {
    std::string_view label;
    std::string_view detail;
};

struct EditorSceneViewState {
    EditorViewport viewport;
    ListViewport popup;
};

struct EditorSceneLayoutInput {
    const Text& text;
    TextOffset caret;
    int rows = 24;
    int cols = 80;
    float visible_text_rows = 0.0F;
    int tab_width = 4;
    bool reveal_caret = true;
    std::size_t popup_item_count = 0;
    // Candidate rows reserved by the active popup. Zero derives the capacity
    // from popup_item_count; pickers provide a stable nonzero value so their
    // geometry remains unchanged while filtering to fewer or zero items.
    std::size_t popup_capacity = 0;
    std::optional<std::size_t> popup_selection;
};

struct EditorSceneInput {
    const Text& text;
    const TokenBuffer& tokens;
    const LineSigns& signs;
    TextOffset caret;
    std::span<const TextRange> selections;
    std::span<const PositionHint> position_hints;

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
    CursorShape cursor_shape = CursorShape::Beam;
    std::string_view input_state_indicator;
    // In-progress key sequence prefix (e.g. "C-x"). Pixel frontends echo it
    // at the message line's right edge; empty when no sequence is pending.
    std::string_view pending_key;
    std::string_view echo;
    // A present value puts the caret on the echo line at this zero-based cell.
    std::optional<int> echo_cursor_column;
    // UTF-8 byte offset in `echo`, used by pixel frontends to shape the caret
    // with the same text layout that they paint.
    std::optional<std::size_t> echo_cursor_byte;

    std::string_view popup_title;
    std::span<const EditorPopupItem> popup_items;
    std::size_t popup_capacity = 0;
    std::optional<std::size_t> popup_selection;
    // Present for an interactive picker. The popup is the sole presentation
    // owner of this input and its caret in every frontend.
    std::optional<std::string_view> popup_input;
    std::optional<std::size_t> popup_input_cursor;
};

struct EditorPaneScene {
    std::string id;
    Rect rect;
    bool active = false;
    Scene scene;
};

struct EditorWorkspaceGeometry {
    int rows = 0;
    int cols = 0;
};

// Resolves caret reveal and popup selection into retained view state. The
// operation is pure: callers explicitly apply the returned state before
// composing one or more frontend frames from it.
EditorSceneViewState layout_editor_scene(const EditorSceneLayoutInput& input,
                                         EditorSceneViewState current);

// Composes the editor's standard five-region frame from immutable model and
// view state. Fractional vertical scroll is represented as a negative grid
// offset, allowing either edge to contain a partial row.
Scene compose_editor_scene(const EditorSceneInput& input, const EditorSceneViewState& view);

// Projects independently composed editor views into one workspace. Pane
// document regions remain independently scrollable inside fixed rectangles;
// the echo area and overlays are taken from the full-frame chrome Scene.
Scene compose_editor_workspace(EditorWorkspaceGeometry geometry, std::vector<EditorPaneScene> panes,
                               std::vector<SceneDivider> dividers, Scene chrome);

} // namespace cind::ui
