#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "gui/editor_model.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace cind;
using namespace cind::gui;

TEST_CASE("wheel scrolling moves the viewport without moving the caret") {
    std::string source;
    for (int line = 0; line < 30; ++line) {
        source += "line\n";
    }
    EditorModel model("sample.cc", source, CppIndentStyle{}, "test", 1);
    model.compose(8, 80);
    const TextOffset caret = model.inspect().caret;

    model.scroll_lines(10);
    const ui::Scene scrolled = model.compose(8, 80);
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.caret == caret);
    CHECK(state.viewport.top_line == 10);
    CHECK_FALSE(scrolled.cursor_visible);

    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 6));
    const ui::Scene revealed = model.compose(8, 80);
    CHECK(model.inspect().caret_position.line == 1);
    CHECK(revealed.cursor_visible);
}

TEST_CASE("caret reveal moves a fractional row to the top edge") {
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\nfour\n", CppIndentStyle{}, "test", 1);
    model.compose(6, 80, 3.5F);

    for (int line = 0; line < 3; ++line) {
        CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 3));
    }

    const ui::Scene scene = model.compose(6, 80, 3.5F);
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.caret_position.line == 3);
    CHECK(state.viewport.top_line == 0);
    CHECK(state.viewport.top_line_offset == doctest::Approx(0.5F));
    CHECK(scene.grid_offset_rows == doctest::Approx(-0.5F));
    CHECK(scene.cursor_visible);
    CHECK(scene.cursor_row == 4);
}

TEST_CASE("cursor movement and deletion operate on extended grapheme clusters") {
    EditorModel model("sample.cc", "e\u0301x", CppIndentStyle{}, "test", 1);

    CHECK(model.handle_key(KeyStroke::named(KeyCode::Right), 10));
    CHECK(model.inspect().caret.value == 3);
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Backspace), 10));
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.document_bytes == 1);
    CHECK(state.caret.value == 0);
}

TEST_CASE("vertical movement preserves the shared preferred display column") {
    EditorModel model("sample.cc", "abcd\nx\nabcd", CppIndentStyle{}, "test", 1);

    for (int index = 0; index < 3; ++index) {
        CHECK(model.handle_key(KeyStroke::named(KeyCode::Right), 10));
    }
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 10));
    CHECK(model.inspect().caret_display_column == 1);
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 10));
    CHECK(model.inspect().caret_display_column == 3);
}

TEST_CASE("search uses the shared non-blocking minibuffer command state") {
    EditorModel model("sample.cc", "one two one", CppIndentStyle{}, "test", 1);

    CHECK(model.handle_key(KeyStroke::character_key(U'f', KeyModifier::Control), 10));
    EditorStateSnapshot state = model.inspect();
    CHECK(state.command_loop.minibuffer.active);
    CHECK(state.command_loop.minibuffer.prompt == "search: ");
    CHECK(state.command_loop.last_command == "search.prompt");

    model.insert_text("two");
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Enter), 10));
    state = model.inspect();
    CHECK_FALSE(state.command_loop.minibuffer.active);
    CHECK(state.command_loop.last_command == "search.accept");
    CHECK(state.caret.value == 4);
}

TEST_CASE("background save captures one revision without blocking newer edits") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("cind-editor-save-{}.cc", static_cast<long>(::getpid()));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    {
        EditorModel model(path.string(), "old", CppIndentStyle{}, "test", 1);
        model.insert_text("x");
        CHECK(model.handle_key(KeyStroke::character_key(U's', KeyModifier::Control), 10));
        model.insert_text("y");

        for (int attempt = 0; attempt < 200 && model.has_background_work(); ++attempt) {
            model.poll_background_work();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE_FALSE(model.has_background_work());
        CHECK(model.inspect().dirty);
    }

    std::ifstream input(path, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    CHECK(saved == "xold");
    std::filesystem::remove(path, ignored);
}
