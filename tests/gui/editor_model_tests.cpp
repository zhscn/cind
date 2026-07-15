#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "gui/editor_model.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
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

    CHECK(model.handle_key(EditorKey::Down, {}, 6));
    const ui::Scene revealed = model.compose(8, 80);
    CHECK(model.inspect().caret_position.line == 1);
    CHECK(revealed.cursor_visible);
}

TEST_CASE("cursor movement and deletion operate on extended grapheme clusters") {
    EditorModel model("sample.cc", "e\u0301x", CppIndentStyle{}, "test", 1);

    CHECK(model.handle_key(EditorKey::Right, {}, 10));
    CHECK(model.inspect().caret.value == 3);
    CHECK(model.handle_key(EditorKey::Backspace, {}, 10));
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.document_bytes == 1);
    CHECK(state.caret.value == 0);
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
        CHECK(model.handle_key(EditorKey::S, {.control = true}, 10));
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
