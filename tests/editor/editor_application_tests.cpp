#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/editor_application.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace cind;

namespace {

EditorApplication make_application(std::string path, std::string initial,
                                   EditorPlatformServices platform_services = {}) {
    return EditorApplication({.path = std::move(path),
                              .initial_text = std::move(initial),
                              .style = {},
                              .style_origin = "test",
                              .initial_line = 0,
                              .platform_services = std::move(platform_services)});
}

void send_keys(EditorApplication& application, std::string_view notation) {
    const std::expected<KeySequence, KeyParseError> sequence = parse_key_sequence(notation);
    REQUIRE(sequence.has_value());
    for (const KeyStroke key : *sequence) {
        CHECK(application.handle_key(key, 10));
    }
}

} // namespace

TEST_CASE("editor application owns normalized interaction dispatch") {
    EditorApplication application = make_application("sample.cc", "one two one");

    send_keys(application, "C-s");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->request.prompt == "search: ");
    CHECK(application.last_command() == "search.prompt");

    application.insert_text("two");
    CHECK(application.handle_key(KeyStroke::named(KeyCode::Enter), 10));
    CHECK_FALSE(application.interaction().active());
    CHECK(application.last_command() == "search.accept");
    CHECK(application.session().caret().value == 4);
}

TEST_CASE("frontend commands join the shared default keymap") {
    EditorApplication application = make_application("sample.cc", "text");
    bool called = false;
    application.runtime().commands().define(
        "search.replace", [&](CommandContext&, const CommandInvocation&) -> CommandResult {
            called = true;
            return CommandCompleted{};
        });
    application.refresh_default_keymap();

    send_keys(application, "M-%");
    CHECK(called);
    CHECK(application.last_command() == "search.replace");
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
        send_keys(application, "C-x C-s");
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

TEST_CASE("default keymap follows Emacs movement search undo and prefix conventions") {
    EditorApplication application = make_application("sample.cc", "one two one");

    send_keys(application, "C-f");
    CHECK(application.session().caret().value == 1);
    send_keys(application, "C-s");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->request.prompt == "search: ");
    send_keys(application, "C-g");
    CHECK_FALSE(application.interaction().active());

    send_keys(application, "C-r");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->request.prompt == "search backward: ");
    send_keys(application, "C-g");

    application.insert_text("x");
    CHECK(application.dirty());
    send_keys(application, "C-/");
    CHECK(application.session().snapshot().content().to_string() == "one two one");

    send_keys(application, "C-x");
    CHECK(application.command_loop().pending_sequence_text() == "C-x");
    const std::vector<KeyBindingHint> hints = application.pending_key_hints();
    CHECK(std::ranges::any_of(hints, [](const KeyBindingHint& hint) {
        return hint.key == "C-s" && hint.command == "file.save";
    }));
    send_keys(application, "C-g");
    CHECK(application.command_loop().pending_sequence().empty());
}

TEST_CASE("which-key help and command palette use searchable interaction providers") {
    EditorApplication application = make_application("sample.cc", "text");

    send_keys(application, "C-h b");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->request.provider == "key-bindings");
    CHECK(std::ranges::any_of(
        application.interaction().state()->candidates, [](const InteractionCandidate& candidate) {
            return candidate.label == "C-x C-s" && candidate.detail == "file.save";
        }));
    send_keys(application, "C-g");

    send_keys(application, "M-x");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->request.provider == "commands");
    application.insert_text("buffer next");
    REQUIRE_FALSE(application.interaction().state()->candidates.empty());
    CHECK(application.interaction().state()->candidates.front().value == "buffer.next");
}

TEST_CASE("Emacs mark kill yank and structural commands are frontend independent") {
    EditorApplication application = make_application("sample.cc", "abc");

    send_keys(application, "C-SPC");
    send_keys(application, "C-f");
    REQUIRE(application.session().selection().has_value());
    CHECK(*application.session().selection() == make_range(0, 1));
    send_keys(application, "C-w");
    CHECK(application.session().snapshot().content().to_string() == "bc");
    send_keys(application, "C-y");
    CHECK(application.session().snapshot().content().to_string() == "abc");

    application.session().set_caret(TextOffset{0});
    application.insert_text("(x) ");
    application.session().set_caret(TextOffset{0});
    send_keys(application, "C-M-f");
    CHECK(application.session().caret().value == 3);
}

TEST_CASE("copy and kill commands synchronize with the platform clipboard") {
    std::string clipboard;
    EditorPlatformServices services{
        .write_clipboard = [&clipboard](std::string_view text) -> std::expected<void, std::string> {
            clipboard = text;
            return {};
        },
        .read_clipboard = [&clipboard]() -> std::expected<std::string, std::string> {
            return clipboard;
        }};
    EditorApplication application = make_application("sample.cc", "abc", std::move(services));

    send_keys(application, "C-SPC C-f M-w");
    CHECK(clipboard == "a");
    CHECK(application.session().snapshot().content().to_string() == "abc");
    send_keys(application, "C-y");
    CHECK(application.session().snapshot().content().to_string() == "aabc");

    application.session().set_caret(TextOffset{0});
    send_keys(application, "C-SPC C-f C-w");
    CHECK(clipboard == "a");
    CHECK(application.session().snapshot().content().to_string() == "abc");
}

TEST_CASE("yank imports the platform clipboard when the internal kill slot is empty") {
    EditorPlatformServices services{
        .write_clipboard = {},
        .read_clipboard = []() -> std::expected<std::string, std::string> { return "outside"; }};
    EditorApplication application = make_application("sample.cc", "text", std::move(services));

    send_keys(application, "C-y");
    CHECK(application.session().snapshot().content().to_string() == "outsidetext");
}

TEST_CASE("soft delete allows malformed literals and exposes raw deletion") {
    EditorApplication malformed = make_application("sample.cc", "#include \"foo\"\n\"");
    malformed.session().set_caret(TextOffset{16});
    send_keys(malformed, "Backspace");
    CHECK(malformed.session().snapshot().content().to_string() == "#include \"foo\"\n");

    EditorApplication balanced = make_application("sample.cc", "\"foo\"");
    balanced.session().set_caret(TextOffset{5});
    send_keys(balanced, "Backspace");
    CHECK(balanced.session().snapshot().content().to_string() == "\"foo\"");
    CHECK(balanced.session().caret().value == 4);

    balanced.session().set_caret(TextOffset{5});
    send_keys(balanced, "C-u Backspace");
    CHECK(balanced.session().snapshot().content().to_string() == "\"foo");

    EditorApplication unmatched_bracket = make_application("sample.cc", "(");
    unmatched_bracket.session().set_caret(TextOffset{1});
    send_keys(unmatched_bracket, "Backspace");
    CHECK(unmatched_bracket.session().snapshot().content().to_string().empty());
}

TEST_CASE("Tab moves blank lines to their contextual indentation") {
    EditorApplication empty = make_application("sample.cc", "void f() {\n\n}\n");
    empty.session().set_caret(TextOffset{11});
    send_keys(empty, "TAB");
    CHECK(empty.session().snapshot().content().to_string() == "void f() {\n    \n}\n");
    CHECK(empty.session().caret().value == 15);

    EditorApplication indented = make_application("sample.cc", "void f() {\n    \n}\n");
    indented.session().set_caret(TextOffset{11});
    const RevisionId revision = indented.revision();
    indented.hide_caret();
    send_keys(indented, "TAB");
    CHECK(indented.revision() == revision);
    CHECK(indented.session().snapshot().content().to_string() == "void f() {\n    \n}\n");
    CHECK(indented.session().caret().value == 15);
    CHECK(indented.reveal_caret());
}

TEST_CASE("buffers retain independent document view and lifecycle state") {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        std::format("cind-buffers-{}", static_cast<long>(::getpid()));
    const std::filesystem::path first_path = directory / "first.cc";
    const std::filesystem::path second_path = directory / "second.cc";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    {
        std::ofstream second(second_path, std::ios::binary);
        second << "second";
    }

    EditorApplication application = make_application(first_path.string(), "first");
    const BufferId first = application.buffer_id();
    application.insert_text("A");
    application.session().view().viewport().top_line_offset = 0.25F;

    const std::expected<BufferId, std::string> opened = application.open_file(second_path.string());
    REQUIRE(opened.has_value());
    const BufferId second = *opened;
    CHECK(second != first);
    application.insert_text("B");
    application.session().view().viewport().left_column = 3;

    REQUIRE(application.switch_buffer(first));
    CHECK(application.session().snapshot().content().to_string() == "Afirst");
    CHECK(application.session().caret().value == 1);
    CHECK(application.session().view().viewport().top_line_offset == doctest::Approx(0.25F));
    REQUIRE(application.switch_buffer(second));
    CHECK(application.session().snapshot().content().to_string() == "Bsecond");
    CHECK(application.session().view().viewport().left_column == 3);

    const std::expected<BufferId, std::string> reused = application.open_file(second_path.string());
    REQUIRE(reused.has_value());
    CHECK(*reused == second);
    CHECK(application.buffer_count() == 2);
    CHECK_FALSE(application.kill_buffer(second).has_value());
    CHECK(application.kill_buffer(second, true).has_value());
    CHECK(application.buffer_count() == 1);
    CHECK(application.buffer_id() == first);

    CHECK(application.kill_buffer(first, true).has_value());
    CHECK(application.buffer_count() == 1);
    CHECK(application.session().buffer().name() == "*scratch*");
    CHECK_FALSE(application.dirty());

    std::filesystem::remove_all(directory, ignored);
}
