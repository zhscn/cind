#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/editor_application.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace cind;

namespace {

EditorApplication make_application(std::string path, std::string initial) {
    return EditorApplication({.path = std::move(path),
                              .initial_text = std::move(initial),
                              .style = {},
                              .style_origin = "test",
                              .initial_line = 0});
}

} // namespace

TEST_CASE("editor application owns normalized input and minibuffer dispatch") {
    EditorApplication application = make_application("sample.cc", "one two one");

    CHECK(application.handle_key(KeyStroke::character_key(U'f', KeyModifier::Control), 10));
    REQUIRE(application.command_loop().minibuffer() != nullptr);
    CHECK(application.command_loop().minibuffer()->request.prompt == "search: ");
    CHECK(application.last_command() == "search.prompt");

    application.insert_text("two");
    CHECK(application.handle_key(KeyStroke::named(KeyCode::Enter), 10));
    CHECK_FALSE(application.command_loop().minibuffer_active());
    CHECK(application.last_command() == "search.accept");
    CHECK(application.session().caret().value == 4);
}

TEST_CASE("frontend commands join the shared default keymap") {
    EditorApplication application = make_application("sample.cc", "text");
    bool called = false;
    application.runtime().commands().define(
        "editor.position", [&](CommandContext&, const CommandInvocation&) -> CommandResult {
            called = true;
            return CommandCompleted{};
        });
    application.refresh_default_keymap();

    CHECK(application.handle_key(KeyStroke::character_key(U'c', KeyModifier::Control), 10));
    CHECK(called);
    CHECK(application.last_command() == "editor.position");
}

TEST_CASE("background saving is independent of a graphical event loop") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("cind-application-save-{}.cc", static_cast<long>(::getpid()));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    {
        EditorApplication application = make_application(path.string(), "old");
        application.insert_text("x");
        CHECK(application.handle_key(KeyStroke::character_key(U's', KeyModifier::Control), 10));
        application.insert_text("y");

        for (int attempt = 0; attempt < 200 && application.has_background_work(); ++attempt) {
            (void)application.poll_background_work();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE_FALSE(application.has_background_work());
        CHECK(application.dirty());
    }

    std::ifstream input(path, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    CHECK(saved == "xold");
    std::filesystem::remove(path, ignored);
}
