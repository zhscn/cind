#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cpp_lexer/lexer.hpp"
#include "ui/ansi_renderer.hpp"
#include "ui/char_width.hpp"
#include "ui/compose_line.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"
#include "ui/list_view.hpp"
#include "ui/scene_damage.hpp"
#include "ui/text_position.hpp"
#include "ui/view_tree.hpp"

#include <algorithm>
#include <span>
#include <string>

using namespace cind;
using namespace cind::ui;

namespace {

PresentationTheme test_theme() {
    return {.canvas = 0xFF1E1E2E,
            .highlight = 0xFF2A2B3C,
            .band = 0xFF313244,
            .selection = 0xFF45475A,
            .divider = 0xFF11111B,
            .text = 0xFFCDD6F4,
            .strong = 0xFFDEE4F7,
            .faded = 0xFF7F849C,
            .faint = 0xFF6C7086,
            .salient = 0xFF89B4FA,
            .popout = 0xFFFAB387,
            .critical = 0xFFF9E2AF,
            .cursor = 0xFFF5E0DC,
            .sign_added = 0xFFA6E3A1,
            .sign_modified = 0xFFF9E2AF,
            .sign_deleted = 0xFFF38BA8};
}

// Lex a snippet and lay out its single line with the given viewport.
std::vector<Run> runs_of(const std::string& text, int left_col = 0, int width = 80,
                         std::optional<TextRange> selection = std::nullopt, int tab_width = 4) {
    static std::vector<TokenBuffer> keepalive; // tokens must outlive the runs
    keepalive.emplace_back(lex(Text(text)).tokens);
    return build_line_runs({.text = text,
                            .start_offset = 0,
                            .tab_width = tab_width,
                            .left_col = left_col,
                            .width = width,
                            .selections = selection ? std::span<const TextRange>(&*selection, 1)
                                                    : std::span<const TextRange>()},
                           keepalive.back());
}

std::string flat_text(const std::vector<Run>& runs) {
    std::string out;
    for (const Run& r : runs) {
        out += r.text;
    }
    return out;
}

} // namespace

TEST_CASE("list viewport retains its origin while selection remains visible") {
    ListViewport viewport;
    constexpr std::size_t capacity = 12;

    for (std::size_t selection = 0; selection <= capacity; ++selection) {
        viewport.reveal(selection, 30, capacity);
    }
    CHECK(viewport.first_item() == 1);

    viewport.reveal(capacity - 1, 30, capacity);
    CHECK(viewport.first_item() == 1);

    viewport.reveal(0, 30, capacity);
    CHECK(viewport.first_item() == 0);
}

TEST_CASE("editor scene layout is explicit and composition preserves view state") {
    const Text text("zero\none\ntwo\nthree\nfour\n");
    const LexOutput lexed = lex(text);
    const TokenBuffer tokens(lexed.tokens);
    const LineSigns signs;
    EditorSceneViewState current{
        .viewport = {.top_line = 3, .top_line_offset = 0.5F, .left_column = 4},
        .popup = {},
    };
    current.popup.reveal(8, 20, 4);

    const EditorSceneViewState laid_out = layout_editor_scene({.text = text,
                                                               .caret = TextOffset{0},
                                                               .rows = 6,
                                                               .cols = 40,
                                                               .visible_text_rows = 3.5F,
                                                               .tab_width = 4,
                                                               .reveal_caret = true,
                                                               .popup_item_count = 20,
                                                               .popup_selection = 8},
                                                              current);
    CHECK(current.viewport.top_line == 3);
    CHECK(current.viewport.top_line_offset == doctest::Approx(0.5F));
    CHECK(laid_out.viewport.top_line == 0);
    CHECK(laid_out.viewport.top_line_offset == doctest::Approx(0.0F));

    const std::vector<ChromeItem> items(20, ChromeItem{.label = "command", .detail = "command"});
    const ModelineContent modeline{.segments = {{.text = "sample.cc", .group = ModelineGroup::Left},
                                                {.text = "N", .group = ModelineGroup::Right}}};
    const EditorSceneInput input{.text = text,
                                 .tokens = tokens,
                                 .signs = signs,
                                 .caret = TextOffset{0},
                                 .selections = {},
                                 .position_hints = {},
                                 .rows = 6,
                                 .cols = 40,
                                 .visible_text_rows = 3.5F,
                                 .tab_width = 4,
                                 .revision = 0,
                                 .modeline = modeline,
                                 .cursor_shape = CursorShape::Block,
                                 .pending_key = "C-x",
                                 .echo = "hello",
                                 .echo_cursor_column = std::nullopt,
                                 .echo_cursor_byte = std::nullopt,
                                 .popup_title = "Command",
                                 .popup_items = items,
                                 .popup_selection = 8,
                                 .popup_input = std::nullopt,
                                 .popup_input_cursor = std::nullopt};
    const Scene first = compose_editor_scene(input, laid_out);
    const Scene second = compose_editor_scene(input, laid_out);
    CHECK(first.grid_offset_rows == doctest::Approx(second.grid_offset_rows));
    CHECK(laid_out.viewport.top_line == 0);
    // A 6-row frame reflows to a single-candidate minibuffer, so the
    // selection scrolls to the top of the window.
    CHECK(laid_out.popup.first_item() == 8);
    const Region* status = first.find(RegionRole::StatusBar);
    const Region* echo = first.find(RegionRole::EchoArea);
    const Region* popup = first.find(RegionRole::Popup);
    REQUIRE(status != nullptr);
    REQUIRE(echo != nullptr);
    REQUIRE(popup != nullptr);
    CHECK(status->primitives().empty());
    CHECK(echo->primitives().empty());
    CHECK(popup->primitives().empty());
    REQUIRE(status->status() != nullptr);
    REQUIRE(echo->echo() != nullptr);
    REQUIRE(popup->popup() != nullptr);
    CHECK(first.cursor_shape == CursorShape::Block);
    CHECK(status->status()->segments.back().text == "N");
    CHECK(render_ansi(first, test_theme()).find("sample.cc") != std::string::npos);
    CHECK(render_ansi(first, test_theme()).find("hello") != std::string::npos);
    CHECK(render_ansi(first, test_theme()).find("command") != std::string::npos);
    CHECK(render_ansi(first, test_theme()).find("\x1b[2 q") != std::string::npos);
}

TEST_CASE("char width: ascii, CJK, combining marks, invalid bytes") {
    CHECK(code_point_width(U'a') == 1);
    CHECK(code_point_width(U'中') == 2);
    CHECK(code_point_width(U'。') == 2);
    CHECK(code_point_width(0x0301) == 0); // combining acute
    CHECK(display_width("abc") == 3);
    CHECK(display_width("中文") == 4);
    CHECK(display_width("a中b") == 4);
    CHECK(display_width("e\u0301") == 1);
    CHECK(display_width("👩‍💻") == 2);
    CHECK(display_width("🇨🇳") == 2);
    CHECK(clip_to_display_width("a中b", 3) == "a中");
    // Invalid UTF-8 decodes byte-by-byte as U+FFFD, width 1 each.
    CHECK(decode_utf8("\x80").bytes == 1);
    CHECK(decode_utf8("\x80").cp == 0xFFFD);
    CHECK(decode_utf8("\xE4\xB8").bytes == 1); // truncated 3-byte sequence
}

TEST_CASE("view tree resolves backend geometry into semantic editor targets") {
    const Text text("zero\none\ntwo\nthree\n");
    const TokenBuffer tokens(lex(text).tokens);
    const std::vector<ChromeItem> popup_items{
        {.label = "first", .detail = "command"},
        {.label = "second", .detail = "command"},
    };
    const EditorSceneViewState view{
        .viewport = {.top_line = 1, .top_line_offset = 0.0F, .left_column = 4},
        .popup = {},
    };
    const ModelineContent modeline{
        .segments = {{.text = "sample.cc", .group = ModelineGroup::Left}}};
    const Scene scene = compose_editor_scene({.text = text,
                                              .tokens = tokens,
                                              .signs = {},
                                              .caret = TextOffset{4},
                                              .selections = {},
                                              .position_hints = {},
                                              .rows = 8,
                                              .cols = 80,
                                              .visible_text_rows = 6.0F,
                                              .tab_width = 4,
                                              .revision = 3,
                                              .modeline = modeline,
                                              .pending_key = {},
                                              .echo = {},
                                              .echo_cursor_column = std::nullopt,
                                              .echo_cursor_byte = std::nullopt,
                                              .popup_title = "Command",
                                              .popup_items = popup_items,
                                              .popup_selection = 1,
                                              .popup_input = std::nullopt,
                                              .popup_input_cursor = std::nullopt},
                                             view);
    const ViewTree tree(scene);
    CHECK(tree.layer(ViewLayer::Grid).children.size() == 3);
    // The minibuffer band is bottom-anchored chrome, not an overlay.
    CHECK(tree.layer(ViewLayer::Chrome).children.size() == 3);
    CHECK(tree.layer(ViewLayer::Overlay).children.empty());

    const ViewNode& document = tree.layer(ViewLayer::Grid).children.back();
    const std::optional<HitTarget> document_target = resolve_hit_target(
        scene, {.region_index = document.region_index,
                .scene_cell = CellPoint{.row = 2, .column = document.rect.col + 3},
                .local_cell = CellPoint{.row = 2, .column = 3},
                .content_index = std::nullopt});
    REQUIRE(document_target);
    CHECK(document_target->kind == HitTargetKind::DocumentText);
    CHECK(document_target->document_line == 3);
    CHECK(document_target->display_column == 7);

    const ViewNode& gutter = tree.layer(ViewLayer::Grid).children.front();
    const std::optional<HitTarget> gutter_target =
        resolve_hit_target(scene, {.region_index = gutter.region_index,
                                   .scene_cell = CellPoint{.row = 1, .column = 0},
                                   .local_cell = CellPoint{.row = 1, .column = 0},
                                   .content_index = std::nullopt});
    REQUIRE(gutter_target);
    CHECK(gutter_target->kind == HitTargetKind::DocumentGutter);
    CHECK(gutter_target->document_line == 2);

    const ViewNode& popup = tree.layer(ViewLayer::Chrome).children.back();
    const std::optional<HitTarget> popup_target =
        resolve_hit_target(scene, {.region_index = popup.region_index,
                                   .scene_cell = std::nullopt,
                                   .local_cell = std::nullopt,
                                   .content_index = 2});
    REQUIRE(popup_target);
    CHECK(popup_target->kind == HitTargetKind::PopupItem);
    CHECK(popup_target->popup_item == 1);
}

TEST_CASE("position hints replace document cells without changing text layout") {
    const Text text("a中b\n");
    const TokenBuffer tokens(lex(text).tokens);
    const std::vector<PositionHint> hints{{.position = TextOffset{1}, .label = "1"}};
    const ModelineContent modeline{
        .segments = {{.text = "sample.cc", .group = ModelineGroup::Left}}};
    const Scene scene = compose_editor_scene({.text = text,
                                              .tokens = tokens,
                                              .signs = {},
                                              .caret = TextOffset{4},
                                              .selections = {},
                                              .position_hints = hints,
                                              .rows = 4,
                                              .cols = 40,
                                              .visible_text_rows = 2.0F,
                                              .tab_width = 4,
                                              .revision = 1,
                                              .modeline = modeline,
                                              .pending_key = {},
                                              .echo = {},
                                              .echo_cursor_column = std::nullopt,
                                              .echo_cursor_byte = std::nullopt,
                                              .popup_title = {},
                                              .popup_items = {},
                                              .popup_selection = std::nullopt,
                                              .popup_input = std::nullopt,
                                              .popup_input_cursor = std::nullopt},
                                             {});

    const Region* body = scene.find(RegionRole::TextArea);
    REQUIRE(body != nullptr);
    const auto hint = std::ranges::find_if(body->primitives(), [](const Prim& primitive) {
        return primitive.kind == PrimKind::PositionHint;
    });
    REQUIRE(hint != body->primitives().end());
    CHECK(hint->row == 0);
    CHECK(hint->col == 1);
    CHECK(hint->text == "1");
    CHECK(hint->style == StyleClass::PositionHint);
    CHECK(hint->span_cols == 2);
    CHECK(hint->id == "hint:0/byte:1");
    CHECK(render_ansi(scene, test_theme()).find("1 \x1b[0m") != std::string::npos);
}

TEST_CASE("view layers define paint order independently of region storage order") {
    Scene scene;
    scene.rows = 1;
    scene.cols = 8;
    scene.cursor_visible = false;
    Region overlay{RegionRole::Popup,       {0, 0, 1, 8}, {}, SurfaceClass::Status,
                   VerticalAnchor::Overlay, "overlay"};
    overlay.primitives().push_back({0, 0, "top", StyleClass::Popup, false});
    Region document{RegionRole::TextArea, {0, 0, 1, 8},         {},
                    SurfaceClass::Editor, VerticalAnchor::Grid, "document"};
    document.primitives().push_back({0, 0, "body", StyleClass::Text, false});
    scene.regions = {overlay, document};

    const std::string rendered = render_ansi(scene, test_theme());
    CHECK(rendered.find("body") < rendered.find("top"));

    SceneDamageTracker tracker;
    REQUIRE(tracker.update(scene).full_repaint);
    scene.regions[1].primitives().front().text = "xxxx";
    const SceneDamage obscured_change = tracker.update(scene);
    CHECK_FALSE(obscured_change.full_repaint);
    CHECK(obscured_change.cell_rects.empty());

    std::swap(scene.regions[0], scene.regions[1]);
    const SceneDamage reordered = tracker.update(scene);
    CHECK_FALSE(reordered.full_repaint);
    CHECK(reordered.cell_rects.empty());
}

TEST_CASE("text positions follow extended grapheme boundaries") {
    const Text text("e\u0301👩‍💻x");
    CHECK(next_grapheme(text, TextOffset{0}).value == 3);
    CHECK(next_grapheme(text, TextOffset{3}).value == 14);
    CHECK(previous_grapheme(text, TextOffset{14}).value == 3);
    CHECK(previous_grapheme(text, TextOffset{3}).value == 0);
}

TEST_CASE("line runs: token styles split the line") {
    const std::vector<Run> runs = runs_of("if (x) return 1;");
    REQUIRE(!runs.empty());
    CHECK(flat_text(runs) == "if (x) return 1;");
    CHECK(runs.front().style == StyleClass::Keyword); // `if`
    CHECK(runs.front().col == 0);
    bool saw_number = false;
    for (const Run& r : runs) {
        if (r.style == StyleClass::Number) {
            CHECK(r.text == "1");
            saw_number = true;
        }
    }
    CHECK(saw_number);
}

TEST_CASE("line runs: tabs expand against the column grid") {
    const std::vector<Run> runs = runs_of("\tx\ty", 0, 80, std::nullopt, 4);
    // Columns: tab -> 0..3, x at 4, tab -> 5..7, y at 8.
    CHECK(flat_text(runs) == "    x   y");
}

TEST_CASE("line runs: selection splits runs and marks them") {
    const std::vector<Run> runs = runs_of("abcdef", 0, 80, TextRange{TextOffset{2}, TextOffset{4}});
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].text == "ab");
    CHECK(!runs[0].selected);
    CHECK(runs[1].text == "cd");
    CHECK(runs[1].selected);
    CHECK(runs[1].col == 2);
    CHECK(runs[2].text == "ef");
    CHECK(!runs[2].selected);
}

TEST_CASE("line runs: multiple selections share the same composition path") {
    const std::vector<TextRange> selections{{TextOffset{1}, TextOffset{2}},
                                            {TextOffset{4}, TextOffset{6}}};
    const TokenBuffer tokens(lex(Text("abcdef")).tokens);
    const std::vector<Run> runs = build_line_runs({.text = "abcdef",
                                                   .start_offset = 0,
                                                   .tab_width = 4,
                                                   .left_col = 0,
                                                   .width = 80,
                                                   .selections = selections},
                                                  tokens);
    REQUIRE(runs.size() == 4);
    CHECK(runs[0].text == "a");
    CHECK_FALSE(runs[0].selected);
    CHECK(runs[1].text == "b");
    CHECK(runs[1].selected);
    CHECK(runs[2].text == "cd");
    CHECK_FALSE(runs[2].selected);
    CHECK(runs[3].text == "ef");
    CHECK(runs[3].selected);
}

TEST_CASE("line runs: CJK glyphs take two columns and clip correctly") {
    // "中文ab" spans columns [0,2) [2,4) [4] [5].
    const std::vector<Run> full = runs_of("中文ab");
    CHECK(flat_text(full) == "中文ab");

    // Clip [1, 5): the first glyph straddles the left edge -> padded space;
    // `b` sits at column 5, outside the window.
    const std::vector<Run> clipped = runs_of("中文ab", 1, 4);
    REQUIRE(!clipped.empty());
    CHECK(clipped.front().col == 0);
    CHECK(flat_text(clipped) == " 文a");

    // Width 3 from col 0: the second glyph would cross the right edge.
    const std::vector<Run> narrow = runs_of("中文ab", 0, 3);
    CHECK(flat_text(narrow) == "中");
}

TEST_CASE("line runs: horizontal scroll clips at both edges") {
    const std::vector<Run> runs = runs_of("0123456789", 3, 4);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].col == 0);
    CHECK(runs[0].text == "3456");
}

TEST_CASE("line signs: modification, insertion, deletion") {
    Text saved("aaa\nbbb\nccc\nddd\n");

    SUBCASE("no change") {
        CHECK(line_signs(saved, saved).empty());
    }
    SUBCASE("single line modified") {
        const Text now = saved.replace(make_range(4, 7), "BXB");
        const LineSigns signs = line_signs(saved, now);
        CHECK(signs.at(0) == SignKind::None);
        CHECK(signs.at(1) == SignKind::Modified);
        CHECK(signs.at(2) == SignKind::None);
    }
    SUBCASE("whole line inserted") {
        const Text now = saved.insert(TextOffset{4}, "new\n");
        const LineSigns signs = line_signs(saved, now);
        CHECK(signs.at(0) == SignKind::None);
        CHECK(signs.at(1) == SignKind::Added);
        CHECK(signs.at(2) == SignKind::None);
    }
    SUBCASE("several lines inserted mid-line become modified + added") {
        const Text now = saved.replace(make_range(5, 5), "X\nY\nZ");
        const LineSigns signs = line_signs(saved, now);
        CHECK(signs.at(1) == SignKind::Modified);
        CHECK(signs.at(2) == SignKind::Added);
        CHECK(signs.at(3) == SignKind::Added);
        CHECK(signs.at(4) == SignKind::None);
    }
    SUBCASE("whole line deleted marks the boundary") {
        const Text now = saved.erase(make_range(4, 8)); // drop "bbb\n"
        const LineSigns signs = line_signs(saved, now);
        CHECK(signs.at(0) == SignKind::None);
        CHECK(signs.at(1) == SignKind::DeletedAbove);
        CHECK(signs.at(2) == SignKind::None);
    }
    SUBCASE("replace two lines with one") {
        const Text now = saved.replace(make_range(4, 12), "X\n"); // bbb,ccc -> X
        const LineSigns signs = line_signs(saved, now);
        CHECK(signs.at(1) == SignKind::Modified);
        CHECK(signs.at(2) == SignKind::DeletedAbove);
    }
    SUBCASE("edit at end of file") {
        const Text now = saved.insert(saved.end_offset(), "eee\n");
        const LineSigns signs = line_signs(saved, now);
        CHECK(signs.at(4) == SignKind::Added);
        CHECK(signs.at(3) == SignKind::None);
    }
    SUBCASE("far-apart edits collapse into one span, O(1) lookups") {
        Text now = saved.replace(make_range(0, 1), "A");
        now = now.replace(make_range(13, 14), "D"); // first byte of ddd
        const LineSigns signs = line_signs(saved, now);
        CHECK(signs.at(0) == SignKind::Modified);
        CHECK(signs.at(3) == SignKind::Modified);
        CHECK(signs.at(1) == SignKind::Modified); // between-window lines join the span
    }
}

TEST_CASE("line signs: content entered into an empty file is added") {
    const Text empty;

    SUBCASE("last line has content") {
        const LineSigns signs = line_signs(empty, Text("one\ntwo"));
        CHECK(signs.at(0) == SignKind::Added);
        CHECK(signs.at(1) == SignKind::Added);
        CHECK(signs.modified == 0);
        CHECK(signs.added == 2);
    }
    SUBCASE("trailing logical empty line is not marked") {
        const LineSigns signs = line_signs(empty, Text("one\ntwo\n"));
        CHECK(signs.at(0) == SignKind::Added);
        CHECK(signs.at(1) == SignKind::Added);
        CHECK(signs.at(2) == SignKind::None);
        CHECK(signs.modified == 0);
        CHECK(signs.added == 2);
    }
    SUBCASE("returning to empty clears the signs") {
        CHECK(line_signs(empty, empty).empty());
    }
}

TEST_CASE("ansi renderer: regions paint at absolute positions") {
    // Primitive content uses region-local coordinates. Semantic chrome is
    // projected by the renderer.
    Scene scene;
    scene.rows = 5; // 3 text rows + status + echo
    scene.cols = 20;

    Region numbers{RegionRole::LineNumbers, {0, 0, 3, 3}, {}};
    numbers.primitives().push_back({0, 0, " 1 ", StyleClass::Gutter, false});
    Region marks{RegionRole::ChangeSigns, {0, 3, 3, 1}, {}};
    marks.primitives().push_back({0, 0, "▎", StyleClass::SignModified, false});
    Region body{RegionRole::TextArea, {0, 4, 3, 16}, {}};
    body.primitives().push_back({0, 0, "int", StyleClass::Keyword, false});
    body.primitives().push_back({0, 3, " x;", StyleClass::Text, false});
    body.primitives().push_back({1, 0, "~", StyleClass::Gutter, false});
    Region status{RegionRole::StatusBar, {3, 0, 1, 20}, {}};
    status.primitives().push_back({0, 0, " f.cc ", StyleClass::StatusBar, false});
    Region echo{RegionRole::EchoArea, {4, 0, 1, 20}, {}};
    echo.primitives().push_back({0, 0, "hello", StyleClass::Message, false});
    scene.regions = {numbers, marks, body, status, echo};
    scene.cursor_row = 1;
    scene.cursor_col = 5;

    const std::string frame = render_ansi(scene, test_theme());
    CHECK(frame.starts_with("\x1b[?25l\x1b[H\x1b[2J"));
    // Region-local coordinates offset by the region's rect (1-based).
    CHECK(frame.find("\x1b[1;1H\x1b[38;2;108;112;134m 1 ") != std::string::npos);
    CHECK(frame.find("\x1b[1;4H\x1b[38;2;249;226;175m▎") != std::string::npos);
    CHECK(frame.find("\x1b[1;5H\x1b[38;2;137;180;250;1mint") != std::string::npos);
    CHECK(frame.find("\x1b[1;8H") != std::string::npos);
    CHECK(frame.find("\x1b[2;5H\x1b[38;2;108;112;134m~") != std::string::npos);
    CHECK(frame.find("\x1b[4;1H\x1b[38;2;205;214;244m\x1b[48;2;49;50;68m f.cc ") !=
          std::string::npos);
    CHECK(frame.find("\x1b[5;1H\x1b[38;2;205;214;244mhello") != std::string::npos);
    CHECK(frame.ends_with("\x1b[1;5H\x1b[6 q\x1b[?25h"));

    // Selected primitives add reverse video.
    Scene sel = scene;
    sel.regions[2].primitives()[1].selected = true;
    CHECK(render_ansi(sel, test_theme()).find("\x1b[38;2;137;180;250;1m") != std::string::npos);
    CHECK(render_ansi(sel, test_theme()).find("\x1b[7m x;") != std::string::npos);

    // find() locates regions by role.
    CHECK(scene.find(RegionRole::TextArea) != nullptr);
    CHECK(scene.find(RegionRole::TextArea)->rect.col == 4);
}
