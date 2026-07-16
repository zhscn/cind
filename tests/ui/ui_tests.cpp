#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cpp_lexer/lexer.hpp"
#include "ui/ansi_renderer.hpp"
#include "ui/char_width.hpp"
#include "ui/compose_line.hpp"
#include "ui/line_signs.hpp"
#include "ui/list_view.hpp"
#include "ui/text_position.hpp"

#include <string>

using namespace cind;
using namespace cind::ui;

namespace {

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
                            .selection = selection},
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
    // The scene is layout + display lists: regions with rects, primitives in
    // region-local coordinates. The renderer only positions and styles.
    Scene scene;
    scene.rows = 5; // 3 text rows + status + echo
    scene.cols = 20;

    Region numbers{RegionRole::LineNumbers, {0, 0, 3, 3}, {}};
    numbers.prims.push_back({0, 0, " 1 ", StyleClass::Gutter, false});
    Region marks{RegionRole::ChangeSigns, {0, 3, 3, 1}, {}};
    marks.prims.push_back({0, 0, "▎", StyleClass::SignModified, false});
    Region body{RegionRole::TextArea, {0, 4, 3, 16}, {}};
    body.prims.push_back({0, 0, "int", StyleClass::Keyword, false});
    body.prims.push_back({0, 3, " x;", StyleClass::Text, false});
    body.prims.push_back({1, 0, "~", StyleClass::Gutter, false});
    Region status{RegionRole::StatusBar, {3, 0, 1, 20}, {}};
    status.prims.push_back({0, 0, " f.cc ", StyleClass::StatusBar, false});
    Region echo{RegionRole::EchoArea, {4, 0, 1, 20}, {}};
    echo.prims.push_back({0, 0, "hello", StyleClass::Message, false});
    scene.regions = {numbers, marks, body, status, echo};
    scene.cursor_row = 1;
    scene.cursor_col = 5;

    const std::string frame = render_ansi(scene);
    CHECK(frame.starts_with("\x1b[?25l\x1b[H\x1b[2J"));
    // Region-local coordinates offset by the region's rect (1-based).
    CHECK(frame.find("\x1b[1;1H\x1b[90m 1 ") != std::string::npos);   // numbers
    CHECK(frame.find("\x1b[1;4H\x1b[33m▎") != std::string::npos);     // sign strip
    CHECK(frame.find("\x1b[1;5H\x1b[1;34mint") != std::string::npos); // text at rect.col
    CHECK(frame.find("\x1b[1;8H") != std::string::npos);              // second run offset
    CHECK(frame.find("\x1b[2;5H\x1b[90m~") != std::string::npos);     // past-EOF marker
    CHECK(frame.find("\x1b[4;1H\x1b[7m f.cc ") != std::string::npos); // status row
    CHECK(frame.find("\x1b[5;1Hhello") != std::string::npos);         // echo row
    CHECK(frame.ends_with("\x1b[1;5H\x1b[?25h"));

    // Selected primitives add reverse video.
    Scene sel = scene;
    sel.regions[2].prims[1].selected = true;
    CHECK(render_ansi(sel).find("\x1b[1;34m") != std::string::npos);
    CHECK(render_ansi(sel).find("\x1b[7m x;") != std::string::npos);

    // find() locates regions by role.
    CHECK(scene.find(RegionRole::TextArea) != nullptr);
    CHECK(scene.find(RegionRole::TextArea)->rect.col == 4);
}
