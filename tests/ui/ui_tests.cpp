#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cpp_lexer/lexer.hpp"
#include "ui/ansi_renderer.hpp"
#include "ui/char_width.hpp"
#include "ui/compose_line.hpp"
#include "ui/line_signs.hpp"

#include <string>

using namespace cind;
using namespace cind::ui;

namespace {

// Lex a snippet and lay out its single line with the given viewport.
std::vector<Run> runs_of(const std::string& text, int left_col = 0, int width = 80,
                         std::optional<TextRange> selection = std::nullopt,
                         int tab_width = 4) {
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

TEST_CASE("char width: ascii, CJK, combining marks, invalid bytes") {
    CHECK(code_point_width(U'a') == 1);
    CHECK(code_point_width(U'中') == 2);
    CHECK(code_point_width(U'。') == 2);
    CHECK(code_point_width(0x0301) == 0); // combining acute
    CHECK(display_width("abc") == 3);
    CHECK(display_width("中文") == 4);
    CHECK(display_width("a中b") == 4);
    // Invalid UTF-8 decodes byte-by-byte as U+FFFD, width 1 each.
    CHECK(decode_utf8("\x80").bytes == 1);
    CHECK(decode_utf8("\x80").cp == 0xFFFD);
    CHECK(decode_utf8("\xE4\xB8").bytes == 1); // truncated 3-byte sequence
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

TEST_CASE("ansi renderer: golden frame") {
    Scene scene;
    scene.rows = 5; // 3 text rows + status + message
    scene.cols = 20;
    scene.gutter_digits = 2;
    scene.show_signs = true;

    LineView line;
    line.line_no = 0;
    line.sign = SignKind::Modified;
    line.runs.push_back({0, "int", StyleClass::Keyword, false});
    line.runs.push_back({3, " x;", StyleClass::Text, false});
    scene.lines.push_back(line);

    scene.status_left = " f.cc ";
    scene.status_key = "key ";
    scene.message = "hello";
    scene.cursor_row = 1;
    scene.cursor_col = 5;

    const std::string frame = render_ansi(scene);
    // Structure, not aesthetics: home, gutter, sign, styled runs, EOF
    // markers, status fill, message, final cursor park.
    CHECK(frame.starts_with("\x1b[?25l\x1b[H"));
    CHECK(frame.find(" 1 ") != std::string::npos);          // line number
    CHECK(frame.find("\x1b[33m▎") != std::string::npos);    // modified sign
    CHECK(frame.find("\x1b[1;34mint") != std::string::npos); // keyword run
    CHECK(frame.find("\x1b[90m~") != std::string::npos);    // past-EOF rows
    CHECK(frame.find("\x1b[7m f.cc ") != std::string::npos); // status bar
    CHECK(frame.find("hello") != std::string::npos);
    CHECK(frame.ends_with("\x1b[1;5H\x1b[?25h"));

    // Selected run renders with reverse video.
    Scene sel = scene;
    sel.lines[0].runs[1].selected = true;
    CHECK(render_ansi(sel).find("\x1b[7m x;") != std::string::npos);

    // Signs off: the sign glyph disappears.
    Scene plain = scene;
    plain.show_signs = false;
    CHECK(render_ansi(plain).find("▎") == std::string::npos);
}
