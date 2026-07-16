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

TEST_CASE("window commands maintain a split tree and independent view state") {
    EditorApplication application = make_application("sample.cc", "one\ntwo\nthree\n");
    const WindowId first = application.window_id();
    application.session().set_caret(TextOffset{4});
    application.session().view().viewport().top_line_offset = 0.25F;

    send_keys(application, "C-x 3");
    REQUIRE(application.open_windows().size() == 2);
    REQUIRE(application.window_layout().root() != nullptr);
    CHECK_FALSE(application.window_layout().root()->leaf());
    CHECK(application.window_layout().root()->axis == WindowSplitAxis::Columns);
    const WindowId second = application.window_layout().leaves()[1];
    CHECK(application.window_id() == first);
    CHECK(application.session(second).caret() == TextOffset{4});
    CHECK(application.session(second).view().viewport().top_line_offset == doctest::Approx(0.25F));

    send_keys(application, "C-x o");
    CHECK(application.window_id() == second);
    application.session().set_caret(TextOffset{0});
    CHECK(application.session(first).caret() == TextOffset{4});

    send_keys(application, "C-x 2");
    REQUIRE(application.window_layout().leaves().size() == 3);
    REQUIRE(application.window_layout().root()->second != nullptr);
    CHECK(application.window_layout().root()->second->axis == WindowSplitAxis::Rows);

    send_keys(application, "C-x 0");
    CHECK(application.window_layout().leaves().size() == 2);
    CHECK(application.runtime().windows().try_get(second) == nullptr);
    send_keys(application, "C-x 1");
    CHECK(application.window_layout().leaves().size() == 1);
    CHECK(application.open_windows().front().active);
}

TEST_CASE("deleting the sole window preserves the application focus target") {
    EditorApplication application = make_application("sample.cc", "text");
    const WindowId window = application.window_id();

    send_keys(application, "C-x 0");
    CHECK(application.window_id() == window);
    CHECK(application.open_windows().size() == 1);
    CHECK(application.message() == "cannot delete the only window");
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
    CHECK(std::ranges::any_of(
        application.interaction().state()->candidates, [](const InteractionCandidate& candidate) {
            return candidate.label == "C-x C-c" && candidate.detail == "application.quit";
        }));
    send_keys(application, "C-g");

    send_keys(application, "M-x");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->request.provider == "commands");
    REQUIRE(application.interaction().state()->candidates.size() > 2);
    CHECK(application.input_focus() == "interaction");
    REQUIRE(application.active_keymap_layers().size() == 2);
    CHECK(application.runtime()
              .keymaps()
              .definition(application.active_keymap_layers().front().keymap)
              .name == "interaction.picker");
    CHECK(application.runtime()
              .keymaps()
              .definition(application.active_keymap_layers().back().keymap)
              .name == "application.global");

    send_keys(application, "C-n");
    CHECK(application.interaction().state()->selected == 1);
    CHECK(application.last_command() == "interaction.next-candidate");
    send_keys(application, "C-p");
    CHECK(application.interaction().state()->selected == 0);
    send_keys(application, "Down");
    CHECK(application.interaction().state()->selected == 1);
    send_keys(application, "Up");
    CHECK(application.interaction().state()->selected == 0);

    application.insert_text("buffer next");
    REQUIRE_FALSE(application.interaction().state()->candidates.empty());
    CHECK(application.interaction().state()->candidates.front().value == "buffer.next");
    send_keys(application, "C-g");
    CHECK_FALSE(application.interaction().active());
    CHECK(application.input_focus() == "window");
    CHECK(application.runtime()
              .keymaps()
              .definition(application.active_keymap_layers().back().keymap)
              .name == "application.global");
}

TEST_CASE("interaction local keymap edits its own input") {
    EditorApplication application = make_application("sample.cc", "text");
    const TextOffset document_caret = application.session().caret();

    send_keys(application, "M-x");
    application.insert_text("abc");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->input.text() == "abc");
    CHECK(application.interaction().state()->input.caret() == 3);

    send_keys(application, "C-b C-b");
    CHECK(application.interaction().state()->input.caret() == 1);
    application.insert_text("X");
    CHECK(application.interaction().state()->input.text() == "aXbc");
    CHECK(application.interaction().state()->input.caret() == 2);
    send_keys(application, "C-f");
    CHECK(application.interaction().state()->input.caret() == 3);
    send_keys(application, "C-a C-d");
    CHECK(application.interaction().state()->input.text() == "Xbc");
    CHECK(application.interaction().state()->input.caret() == 0);
    send_keys(application, "C-e");
    CHECK(application.interaction().state()->input.caret() == 3);
    CHECK(application.session().caret() == document_caret);

    send_keys(application, "C-g");
}

TEST_CASE("application global prefix remains active while picker owns focus") {
    EditorApplication application = make_application("sample.cc", "text");

    send_keys(application, "M-x");
    REQUIRE(application.interaction().state() != nullptr);
    REQUIRE_FALSE(application.interaction().state()->candidates.empty());
    send_keys(application, "C-x");
    REQUIRE(application.command_loop().pending_keymap());
    CHECK(application.runtime()
              .keymaps()
              .definition(*application.command_loop().pending_keymap())
              .name == "application.global");
    send_keys(application, "C-c");
    CHECK(application.should_quit());
}

TEST_CASE("active window assembles explicit window view buffer mode and global keymaps") {
    EditorApplication application = make_application("sample.cc", "text");
    EditorRuntime& runtime = application.runtime();
    int selected_layer = 0;
    struct Layer {
        KeymapId keymap;
        CommandId command;
    };
    const auto define_layer = [&](std::string name, int value) {
        const CommandId command = runtime.commands().define(
            std::move(name),
            [&, value](CommandContext&, const CommandInvocation&) -> CommandResult {
                selected_layer = value;
                return CommandCompleted{};
            });
        const KeymapId map = runtime.keymaps().define(std::format("test.layer.{}", value));
        runtime.keymaps().bind(map, "C-z", command);
        return Layer{.keymap = map, .command = command};
    };

    const Layer global = define_layer("test.global", 1);
    const Layer mode = define_layer("test.mode", 2);
    const Layer buffer = define_layer("test.buffer", 3);
    const Layer view = define_layer("test.view", 4);
    const Layer window = define_layer("test.window", 5);
    runtime.keymaps().bind(application.default_keymap(), "C-z", global.command);
    const ModeId major = application.session().buffer().modes().major().value_or(ModeId{});
    REQUIRE(major);
    runtime.modes().definition_for_configuration(major).keymaps.push_back(mode.keymap);
    application.session().buffer().keymaps().push_back(buffer.keymap);
    application.session().view().keymaps().push_back(view.keymap);
    runtime.windows().get(application.window_id()).keymaps().push_back(window.keymap);

    send_keys(application, "C-z");
    CHECK(selected_layer == 5);
    runtime.windows().get(application.window_id()).keymaps().clear();
    send_keys(application, "C-z");
    CHECK(selected_layer == 4);
    application.session().view().keymaps().clear();
    send_keys(application, "C-z");
    CHECK(selected_layer == 3);
    application.session().buffer().keymaps().clear();
    send_keys(application, "C-z");
    CHECK(selected_layer == 2);
    runtime.modes().definition_for_configuration(major).keymaps.clear();
    send_keys(application, "C-z");
    CHECK(selected_layer == 1);
}

TEST_CASE("minor mode keymaps use reverse activation precedence and sparse fallback") {
    EditorApplication application = make_application("sample.cc", "text");
    EditorRuntime& runtime = application.runtime();
    int selected_mode = 0;
    const auto define_minor = [&](std::string name, int value) {
        const CommandId command = runtime.commands().define(
            name + ".command",
            [&, value](CommandContext&, const CommandInvocation&) -> CommandResult {
                selected_mode = value;
                return CommandCompleted{};
            });
        const KeymapId keymap = runtime.keymaps().define(name + ".map");
        runtime.keymaps().bind(keymap, "C-z", command);
        runtime.keymaps().bind(keymap, value == 1 ? "C-c a" : "C-c b", command);
        const ModeId mode = runtime.modes().define(name, ModeKind::Minor);
        runtime.modes().definition_for_configuration(mode).keymaps.push_back(keymap);
        return mode;
    };
    const ModeId first = define_minor("test.minor.first", 1);
    const ModeId second = define_minor("test.minor.second", 2);
    BufferModes& modes = application.session().buffer().modes();
    REQUIRE(modes.enable_minor(runtime.modes(), first));
    REQUIRE(modes.enable_minor(runtime.modes(), second));

    send_keys(application, "C-z");
    CHECK(selected_mode == 2);
    send_keys(application, "C-c a");
    CHECK(selected_mode == 1);

    send_keys(application, "M-x");
    REQUIRE(application.active_keymap_layers().size() == 2);
    CHECK(application.active_keymap_layers().front().scope == "interaction");
    CHECK(application.active_keymap_layers().back().scope == "global");
    send_keys(application, "C-g");

    REQUIRE(modes.disable_minor(second));
    send_keys(application, "C-z");
    CHECK(selected_mode == 1);
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
