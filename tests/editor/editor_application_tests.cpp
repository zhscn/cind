#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/editor_application.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

using namespace cind;

namespace {

class WakeSignal {
public:
    void notify() {
        {
            std::scoped_lock lock(mutex_);
            notified_ = true;
        }
        changed_.notify_one();
    }

    bool wait() {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(2), [this] { return notified_; })) {
            return false;
        }
        notified_ = false;
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool notified_ = false;
};

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

TEST_CASE("direct text input uses the active strategy selection edit policy") {
    EditorApplication application = make_application("sample.cc", "abcd");
    application.session().set_selection({.anchor = TextOffset{0}, .head = TextOffset{2}});
    application.insert_text("x");
    CHECK_FALSE(application.session().active_selection().has_value());

    EditorRuntime& runtime = application.runtime();
    const InputStateId emacs = runtime.input_states().find("emacs").value_or(InputStateId{});
    REQUIRE(emacs);
    const InputStrategyId preserve =
        runtime.input_strategies().define({.name = "test-preserve-selection",
                                           .editing = emacs,
                                           .interface = emacs,
                                           .selection_after_edit = SelectionEditPolicy::Preserve});
    runtime.views().set_input_strategy(application.view_id(), preserve);
    const ViewSelection selection{.ranges = {{.anchor = TextOffset{0},
                                              .head = TextOffset{2},
                                              .granularity = SelectionGranularity::Character},
                                             {.anchor = TextOffset{3},
                                              .head = TextOffset{4},
                                              .granularity = SelectionGranularity::Node}},
                                  .primary = 0,
                                  .metadata = "(strategy . preserve)"};
    application.session().set_selection(selection);
    application.insert_text("y");

    const ViewSelection expected{.ranges = {{.anchor = TextOffset{0},
                                             .head = TextOffset{3},
                                             .granularity = SelectionGranularity::Character},
                                            {.anchor = TextOffset{4},
                                             .head = TextOffset{5},
                                             .granularity = SelectionGranularity::Node}},
                                 .primary = 0,
                                 .metadata = "(strategy . preserve)"};
    CHECK(application.session().active_selection() == expected);
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

TEST_CASE("application modes join the scripted core hierarchy") {
    EditorApplication application = make_application("sample.cc", "text");
    const ModeRegistry& modes = application.runtime().modes();
    const ModeId cpp = modes.find("cind.cpp").value_or(ModeId{});
    const ModeId prog = modes.find("prog-mode").value_or(ModeId{});
    const ModeId locations = modes.find("cind.location-list").value_or(ModeId{});
    const ModeId special = modes.find("special-mode").value_or(ModeId{});
    REQUIRE(cpp);
    REQUIRE(prog);
    REQUIRE(locations);
    REQUIRE(special);
    CHECK(modes.definition(cpp).parent == prog);
    CHECK(modes.definition(locations).parent == special);
    CHECK(modes.effective_policy(application.session().buffer().modes()).interaction_class ==
          InteractionClass::Editing);
}

TEST_CASE("per-view input states precede window layers and may handle keys") {
    EditorApplication application = make_application("sample.cc", "text");
    EditorRuntime& runtime = application.runtime();
    int selected = 0;
    const auto command = [&](std::string name, int value) {
        return runtime.commands().define(
            std::move(name),
            [&, value](CommandContext&, const CommandInvocation&) -> CommandResult {
                selected = value;
                return CommandCompleted{};
            });
    };
    const CommandId base_command = command("test.state.base", 1);
    const CommandId transient_command = command("test.state.transient", 2);
    const CommandId handled_command = command("test.state.handler", 3);
    WindowId handler_window;
    const KeymapId base_map = runtime.keymaps().define("test.state.base-map");
    const KeymapId transient_map = runtime.keymaps().define("test.state.transient-map");
    runtime.keymaps().bind(base_map, "z", base_command);
    runtime.keymaps().bind(transient_map, "z", transient_command);
    const InputStateId base = runtime.input_states().define(
        {.name = "test-base", .keymaps = {base_map}, .indicator = "B", .handler = {}});
    const InputStateId transient = runtime.input_states().define(
        {.name = "test-transient",
         .keymaps = {transient_map},
         .text_input = TextInputPolicy::Ignore,
         .cursor = CursorShape::Block,
         .indicator = "T",
         .handler = [handled_command, &handler_window](CommandContext& context,
                                                       KeyStroke key) -> InputStateHandlerResult {
             handler_window = context.window_id();
             if (format_key_stroke(key) == "q") {
                 return InputStateHandlerAction{.kind = InputStateHandlerActionKind::Consume,
                                                .command = {},
                                                .feedback = std::nullopt};
             }
             if (format_key_stroke(key) == "d") {
                 return InputStateHandlerAction{.kind = InputStateHandlerActionKind::Dispatch,
                                                .command = handled_command,
                                                .feedback = std::nullopt};
             }
             if (format_key_stroke(key) == "p") {
                 return InputStateHandlerAction{
                     .kind = InputStateHandlerActionKind::Pending,
                     .command = {},
                     .feedback = InputFeedback{
                         .sequence = "C-x",
                         .hints = {{.key = "C-s", .detail = "file.save", .prefix = false},
                                   {.key = "4", .detail = "window", .prefix = true}}}};
             }
             return InputStateHandlerAction{};
         }});
    runtime.views().set_base_input_state(application.view_id(), base);
    runtime.views().push_input_state(application.view_id(), transient);

    CHECK(application.handle_key(KeyStroke::character_key(U'z'), 10));
    CHECK(selected == 2);
    REQUIRE(application.active_keymap_layers().size() >= 2);
    CHECK(application.active_keymap_layers()[0].scope == "input-state:test-transient:transient");
    CHECK(application.active_keymap_layers()[1].scope == "input-state:test-base");
    const std::vector<KeymapLayer> base_layers =
        application.base_keymap_layers(application.window_id());
    CHECK(std::ranges::none_of(base_layers, [](const KeymapLayer& layer) {
        return layer.scope.starts_with("input-state:");
    }));
    CHECK(std::ranges::any_of(base_layers,
                              [](const KeymapLayer& layer) { return layer.scope == "editor"; }));
    CHECK(std::ranges::any_of(base_layers,
                              [](const KeymapLayer& layer) { return layer.scope == "global"; }));
    CHECK(application.handle_key(KeyStroke::character_key(U'd'), 10));
    CHECK(selected == 3);
    CHECK(handler_window == application.window_id());
    CHECK(application.handle_key(KeyStroke::character_key(U'p'), 10));
    CHECK(application.pending_key_sequence_text() == "C-x");
    CHECK(application.pending_input_state_name() == "test-transient");
    CHECK(application.command_loop().pending_sequence().empty());
    CHECK(application.pending_key_hints() ==
          std::vector<KeyBindingHint>{{.key = "C-s", .detail = "file.save", .prefix = false},
                                      {.key = "4", .detail = "window", .prefix = true}});
    CHECK(application.handle_key(KeyStroke::character_key(U'q'), 10));
    CHECK(selected == 3);
    CHECK(application.pending_key_sequence_text().empty());

    CHECK(runtime.views().pop_input_state(application.view_id()) == transient);
    CHECK(application.handle_key(KeyStroke::character_key(U'z'), 10));
    CHECK(selected == 1);
}

TEST_CASE("text input follows the focused input state policy") {
    EditorApplication application = make_application("sample.cc", "text");
    EditorRuntime& runtime = application.runtime();
    int invoked = 0;
    const CommandId command = runtime.commands().define(
        "test.normal.x", [&](CommandContext&, const CommandInvocation&) -> CommandResult {
            ++invoked;
            return CommandCompleted{};
        });
    const KeymapId keymap = runtime.keymaps().define("test.normal.map");
    runtime.keymaps().bind(keymap, "x", command);
    const InputStateId normal =
        runtime.input_states().define({.name = "test-normal",
                                       .keymaps = {keymap},
                                       .text_input = TextInputPolicy::Ignore,
                                       .cursor = CursorShape::Block,
                                       .indicator = "N",
                                       .handler = {}});
    runtime.views().set_base_input_state(application.view_id(), normal);

    CHECK(application.text_input_policy() == TextInputPolicy::Ignore);
    CHECK(application.handle_key(KeyStroke::character_key(U'x'), 10));
    application.insert_text("x");
    CHECK(invoked == 1);
    CHECK(application.session().snapshot().content().to_string() == "text");

    CHECK_FALSE(application.handle_key(KeyStroke::character_key(U'z'), 10));
    application.insert_text("z");
    CHECK(application.session().snapshot().content().to_string() == "text");

    send_keys(application, "M-x");
    REQUIRE(application.interaction().active());
    CHECK(application.text_input_policy() == TextInputPolicy::Accept);
    CHECK_FALSE(application.handle_key(KeyStroke::character_key(U'z'), 10));
    application.insert_text("z");
    CHECK(application.interaction().state()->input.text() == "z");
    CHECK(application.session().snapshot().content().to_string() == "text");
}

TEST_CASE("Guile meow keypad translates through base layers and preserves the escape layer") {
    EditorApplication application = make_application("sample.cc", "abcdefghijklmnop");
    EditorRuntime& runtime = application.runtime();

    send_keys(application, "C-c m");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(runtime.input_strategies()
              .definition(runtime.views().get(application.view_id()).input_strategy().value())
              .name == "meow");

    send_keys(application, "1 2");
    CHECK(application.pending_prefix_text() == "12");
    CHECK(application.pending_command_text() == "12");
    CHECK(application.command_loop().pending_prefix().count == 12);
    send_keys(application, "l");
    CHECK(application.session().caret() == TextOffset{12});
    CHECK(application.pending_prefix_text().empty());
    send_keys(application, "0");
    CHECK(application.session().caret() == TextOffset{0});

    send_keys(application, "3");
    send_keys(application, "\"");
    CHECK(application.input_state().name == "meow-register");
    CHECK(application.pending_command_text() == "3 \"");
    send_keys(application, "a");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_command_text() == "3 \"a");
    CHECK(application.command_loop().pending_prefix().register_name ==
          std::optional<std::string>{"a"});
    send_keys(application, "l");
    CHECK(application.session().caret() == TextOffset{3});
    CHECK(application.pending_command_text().empty());

    send_keys(application, "\"");
    CHECK(application.input_state().name == "meow-register");
    send_keys(application, "C-g");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_command_text().empty());

    send_keys(application, "x");
    CHECK(application.input_state().name == "meow-keypad");
    CHECK(application.pending_key_sequence_text() == "C-x");
    CHECK(application.pending_input_state_name() == "meow-keypad");
    CHECK(std::ranges::any_of(application.pending_key_hints(), [](const KeyBindingHint& hint) {
        return hint.key == "C-c" && hint.detail == "application.quit";
    }));
    CHECK(std::ranges::none_of(
        application.base_keymap_layers(application.window_id()),
        [](const KeymapLayer& layer) { return layer.scope.starts_with("input-state:"); }));

    const WindowId keypad_window = application.window_id();
    REQUIRE(application.split_window(WindowSplitAxis::Columns));
    const WindowId other_window = application.window_layout().leaves().back();
    REQUIRE(application.focus_window(other_window));
    CHECK(application.pending_key_sequence_text().empty());
    REQUIRE(application.focus_window(keypad_window));
    CHECK(application.pending_key_sequence_text() == "C-x");

    send_keys(application, "C-g");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_key_sequence_text().empty());
    CHECK_FALSE(application.should_quit());

    send_keys(application, "x c");
    CHECK(application.should_quit());
}

TEST_CASE("Guile meow keypad supports literal and transparent base-layer fallback") {
    EditorApplication application = make_application("sample.cc", "text");
    EditorRuntime& runtime = application.runtime();
    send_keys(application, "C-c m");

    send_keys(application, "x b");
    REQUIRE(application.interaction().active());
    CHECK(application.interaction().state()->request.prompt == "Switch buffer: ");
    send_keys(application, "C-g");

    send_keys(application, "m x");
    REQUIRE(application.interaction().active());
    CHECK(application.interaction().state()->request.prompt == "Command: ");
    send_keys(application, "C-g");

    int transparent_calls = 0;
    const CommandId transparent = runtime.commands().define(
        "test.meow.transparent", [&](CommandContext&, const CommandInvocation&) -> CommandResult {
            ++transparent_calls;
            return CommandCompleted{};
        });
    const KeymapId interface_map = runtime.keymaps().define("test.meow.interface");
    runtime.keymaps().bind(interface_map, "SPC z", transparent);
    application.session().buffer().keymaps().push_back(interface_map);
    const ModeId special = runtime.modes().find("special-mode").value_or(ModeId{});
    REQUIRE(special);
    application.session().buffer().modes().set_major(runtime.modes(), special);
    CHECK(application.input_state().name == "meow-motion");

    send_keys(application, "SPC z");
    CHECK(transparent_calls == 1);
    CHECK(application.input_state().name == "meow-motion");
}

TEST_CASE("Guile meow motions and things consume shared noun registries") {
    EditorApplication application = make_application("sample.cc", "one two three vector<int>");
    send_keys(application, "C-c m");

    send_keys(application, "3 w");
    CHECK(application.session().caret() == TextOffset{14});
    CHECK(application.pending_command_text().empty());

    application.session().set_caret(TextOffset{21});
    send_keys(application, ",");
    CHECK(application.input_state().name == "meow-thing");
    CHECK(application.pending_key_sequence_text() == ",");
    send_keys(application, "a");
    CHECK(application.input_state().name == "meow-normal");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(21, 24));
    CHECK(application.session().active_selection()->ranges.front().granularity ==
          SelectionGranularity::Character);

    send_keys(application, ". a");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(20, 25));
    CHECK(application.session().active_selection()->ranges.front().granularity ==
          SelectionGranularity::Node);
}

TEST_CASE("Guile selection verbs replace every range in one undo unit") {
    EditorApplication application = make_application("sample.cc", "one\ntwo\nthree\n");
    EditorRuntime& runtime = application.runtime();
    runtime.views().set_selection(
        application.view_id(),
        ViewSelection{.ranges = {{.anchor = TextOffset{0},
                                  .head = TextOffset{3},
                                  .granularity = SelectionGranularity::Character},
                                 {.anchor = TextOffset{4},
                                  .head = TextOffset{4},
                                  .granularity = SelectionGranularity::Line}},
                      .primary = 1,
                      .metadata = "(verb . delete)"});

    const std::optional<CommandId> command = runtime.commands().find("edit.delete-selection");
    REQUIRE(command.has_value());
    CommandContext context(runtime, application.window_id(), application.buffer_id(),
                           application.view_id());
    CHECK(application.command_loop().execute(*command, context).status ==
          CommandLoopStatus::Executed);
    CHECK(application.session().snapshot().content() == "\nthree\n");
    const ViewSelection result = application.session().selection_model();
    CHECK(result == ViewSelection{.ranges = {{.anchor = TextOffset{0},
                                              .head = TextOffset{0},
                                              .granularity = SelectionGranularity::Character},
                                             {.anchor = TextOffset{1},
                                              .head = TextOffset{1},
                                              .granularity = SelectionGranularity::Character}},
                                  .primary = 1,
                                  .metadata = "(verb . delete)"});

    CHECK(application.session().undo());
    CHECK(application.session().snapshot().content() == "one\ntwo\nthree\n");
    CHECK_FALSE(application.session().undo());
    CHECK(application.session().redo());
    CHECK(application.session().snapshot().content() == "\nthree\n");
    CHECK_FALSE(application.session().redo());
}

TEST_CASE("background saving is independent of a graphical event loop") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("cind-application-save-{}.cc", static_cast<long>(::getpid()));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    {
        WakeSignal wake;
        EditorApplication application = make_application(
            path.string(), "old",
            {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                 wake.notify();
             }});
        application.insert_text("x");
        send_keys(application, "C-x C-s");
        application.insert_text("y");

        REQUIRE(wake.wait());
        CHECK(application.poll_background_work());
        REQUIRE_FALSE(application.has_background_work());
        CHECK(application.dirty());
    }

    std::ifstream input(path, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    CHECK(saved == "xold");
    std::filesystem::remove(path, ignored);
}

TEST_CASE("scripted save-as policy configures and saves a file buffer") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("cind-application-save-as-{}.cc", static_cast<long>(::getpid()));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    WakeSignal wake;
    EditorApplication application = make_application(
        {}, "contents", {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                             wake.notify();
                         }});
    send_keys(application, "C-x C-w");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().state()->request.prompt == "Write file: ");
    CHECK(application.interaction().state()->input.text().empty());
    application.insert_text(path.string());
    send_keys(application, "RET");
    CHECK(application.last_command() == "file.save");

    REQUIRE(wake.wait());
    CHECK(application.poll_background_work());
    CHECK_FALSE(application.has_background_work());
    CHECK(application.session().buffer().kind() == BufferKind::File);
    CHECK(application.session().buffer().resource_uri() == path.string());
    CHECK(application.session().buffer().name() == path.filename().string());
    std::ifstream input(path, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    CHECK(saved == "contents");

    std::filesystem::remove(path, ignored);
}

TEST_CASE("initial files load through the async runtime and replace the startup scratch buffer") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("cind-application-open-{}.cc", static_cast<long>(::getpid()));
    {
        std::ofstream output(path, std::ios::binary);
        output << "first\nsecond\nthird\n";
    }

    WakeSignal wake;
    EditorApplication application({
        .path = path.string(),
        .initial_text = std::nullopt,
        .style = {},
        .style_origin = "fallback",
        .initial_line = 2,
        .platform_services = {.write_clipboard = {},
                              .read_clipboard = {},
                              .wake_event_loop = [&wake] { wake.notify(); }},
    });
    CHECK(application.session().buffer().kind() == BufferKind::Scratch);
    CHECK(application.has_background_work());

    REQUIRE(wake.wait());
    REQUIRE(application.poll_background_work());
    CHECK_FALSE(application.has_background_work());
    CHECK(application.buffer_count() == 1);
    CHECK(application.session().buffer().kind() == BufferKind::File);
    CHECK(application.session().buffer().resource_uri() == path.string());
    CHECK(application.session().snapshot().content() == "first\nsecond\nthird\n");
    CHECK(application.session().caret().value == 6);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("project discovery indexes files and feeds the project file picker") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        std::format("cind-application-project-{}", static_cast<long>(::getpid()));
    std::filesystem::create_directories(root / ".git");
    std::filesystem::create_directories(root / "src");
    {
        std::ofstream output(root / "src" / "main.cpp");
        output << "int main() {}\n";
    }
    {
        std::ofstream output(root / "src" / "other.cpp");
        output << "zero\n  int other() {}\n";
    }

    {
        WakeSignal wake;
        EditorApplication application({
            .path = (root / "src" / "main.cpp").string(),
            .initial_text = std::nullopt,
            .style = {},
            .style_origin = "fallback",
            .initial_line = 0,
            .platform_services = {.write_clipboard = {},
                                  .read_clipboard = {},
                                  .wake_event_loop = [&wake] { wake.notify(); }},
        });
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }

        const std::optional<ProjectId> project_id = application.session().buffer().project_id();
        REQUIRE(project_id.has_value());
        const Project& project = application.runtime().projects().get(*project_id);
        CHECK(project.roots() == std::vector<std::string>{root.string()});
        CHECK(project.files().size() == 2);

        {
            std::ofstream output(root / "src" / "watched.cpp");
            output << "int watched() {}\n";
        }
        const std::uint64_t original_revision = project.index_revision();
        while (application.runtime().projects().get(*project_id).index_revision() ==
               original_revision) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.runtime().projects().get(*project_id).files().size() == 3);

        send_keys(application, "C-x p f");
        REQUIRE(application.interaction().state() != nullptr);
        CHECK(application.interaction().state()->request.provider == "project-files");
        CHECK(std::ranges::any_of(application.interaction().state()->candidates,
                                  [](const InteractionCandidate& candidate) {
                                      return candidate.label == "src/other.cpp";
                                  }));
        application.insert_text("other");
        send_keys(application, "RET");
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.session().buffer().resource_uri() ==
              (root / "src" / "other.cpp").string());

        send_keys(application, "C-x p g");
        REQUIRE(application.interaction().state() != nullptr);
        CHECK(application.interaction().state()->request.prompt == "Project search: ");
        application.insert_text("int");
        send_keys(application, "RET");
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.session().buffer().kind() == BufferKind::Process);
        CHECK(application.session().buffer().read_only());
        CHECK(application.session().snapshot().content().to_string().find("src/other.cpp") !=
              std::string::npos);
        const Buffer& results = application.session().buffer();
        REQUIRE(results.modes().major().has_value());
        CHECK(application.runtime().modes().definition(*results.modes().major()).name ==
              "cind.location-list");
        CHECK(application.syntax_tokens().empty());
        REQUIRE(results.locations().size() == 3);
        const BufferId result_buffer = results.id();
        const BufferLocation first = results.locations()[0];
        const BufferLocation second = results.locations()[1];
        const BufferLocation third = results.locations()[2];
        CHECK(application.location_navigation().buffer == result_buffer);
        CHECK_FALSE(application.location_navigation().selected_index.has_value());
        send_keys(application, "M-n");
        CHECK(application.session().caret() == second.source_range.start);
        REQUIRE(application.split_window(WindowSplitAxis::Columns));
        const std::vector<OpenWindowSnapshot> windows = application.open_windows();
        const auto secondary = std::ranges::find_if(
            windows, [](const OpenWindowSnapshot& window) { return !window.active; });
        REQUIRE(secondary != windows.end());
        application.session(secondary->window).set_caret(first.source_range.start);
        const std::string result_text = application.session().snapshot().content().to_string();
        application.insert_text("ignored");
        CHECK(application.session().snapshot().content().to_string() == result_text);
        CHECK(application.message() == "buffer is read-only");
        send_keys(application, "RET");
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.session().buffer().resource_uri() == second.resource);
        CHECK(application.session().snapshot().content().position(application.session().caret()) ==
              second.target);
        CHECK(application.location_navigation().selected_index == 1);
        CHECK(application.buffer_id(secondary->window) == result_buffer);
        CHECK(application.session(secondary->window).caret() == first.source_range.start);

        send_keys(application, "M-g n");
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.session().buffer().resource_uri() == third.resource);
        CHECK(application.session().snapshot().content().position(application.session().caret()) ==
              third.target);
        CHECK(application.location_navigation().selected_index == 2);

        send_keys(application, "M-g p");
        CHECK(application.session().buffer().resource_uri() == second.resource);
        CHECK(application.location_navigation().selected_index == 1);
        REQUIRE(application.switch_buffer(result_buffer));
        CHECK(application.session().caret() == second.source_range.start);
        send_keys(application, "C-x `");
        CHECK(application.location_navigation().selected_index == 2);

        REQUIRE(application.kill_buffer(result_buffer, true).has_value());
        CHECK_FALSE(application.location_navigation().buffer.has_value());
    }
    std::filesystem::remove_all(root);
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
        return hint.key == "C-s" && hint.detail == "file.save";
    }));
    send_keys(application, "C-g");
    CHECK(application.command_loop().pending_sequence().empty());
}

TEST_CASE("window commands maintain a split tree and independent view state") {
    EditorApplication application = make_application("sample.cc", "one\ntwo\nthree\n");
    const WindowId first = application.window_id();
    application.session().set_caret(TextOffset{4});
    application.session().view().viewport().top_line_offset = 0.25F;
    const ViewSelection source_selection{
        .ranges = {{.anchor = TextOffset{1},
                    .head = TextOffset{4},
                    .granularity = SelectionGranularity::Character},
                   {.anchor = TextOffset{12},
                    .head = TextOffset{8},
                    .granularity = SelectionGranularity::Node}},
        .primary = 0,
        .metadata = "(thing . expression)"};
    application.session().set_selection(source_selection);

    send_keys(application, "C-x 3");
    REQUIRE(application.open_windows().size() == 2);
    REQUIRE(application.window_layout().root() != nullptr);
    CHECK_FALSE(application.window_layout().root()->leaf());
    CHECK(application.window_layout().root()->axis == WindowSplitAxis::Columns);
    const WindowId second = application.window_layout().leaves()[1];
    CHECK(application.window_id() == first);
    CHECK(application.session(second).caret() == TextOffset{4});
    CHECK(application.session(second).view().viewport().top_line_offset == doctest::Approx(0.25F));
    CHECK(application.session(second).active_selection() == source_selection);

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

TEST_CASE("scripted caret and message commands use application host capabilities") {
    EditorApplication application = make_application("sample.cc", "one\ntwo\nthree\n");
    EditorRuntime& runtime = application.runtime();
    CommandContext context(runtime, application.window_id(), application.buffer_id(),
                           application.view_id());

    const CommandId goto_line =
        runtime.commands().find("cursor.goto-line.accept").value_or(CommandId{});
    REQUIRE(goto_line);
    const CommandResult moved = runtime.commands().invoke(
        goto_line, context, CommandInvocation{.arguments = {std::string("2:2")}, .prefix = {}});
    REQUIRE(moved.has_value());
    CHECK(application.session().caret() == TextOffset{5});
    CHECK(application.reveal_caret());

    const CommandId help = runtime.commands().find("help.keys.accept").value_or(CommandId{});
    REQUIRE(help);
    const CommandResult explained = runtime.commands().invoke(
        help, context,
        CommandInvocation{.arguments = {std::string("C-x C-s  file.save")}, .prefix = {}});
    REQUIRE(explained.has_value());
    CHECK(application.message() == "C-x C-s  file.save");
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
    runtime.modes().add_keymap(major, mode.keymap);
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
    runtime.modes().clear_keymaps(major);
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
        runtime.modes().add_keymap(mode, keymap);
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
        },
        .wake_event_loop = {}};
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
        .read_clipboard = []() -> std::expected<std::string, std::string> { return "outside"; },
        .wake_event_loop = {}};
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

    WakeSignal wake;
    EditorApplication application =
        make_application(first_path.string(), "first",
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    const BufferId first = application.buffer_id();
    application.insert_text("A");
    application.session().view().viewport().top_line_offset = 0.25F;

    const std::expected<void, std::string> opened = application.open_file(second_path.string());
    REQUIRE(opened.has_value());
    REQUIRE(wake.wait());
    REQUIRE(application.poll_background_work());
    const BufferId second = application.buffer_id();
    CHECK(second != first);
    application.insert_text("B");
    application.session().view().viewport().left_column = 3;

    const CommandId switch_buffer =
        application.runtime().commands().find("buffer.switch.accept").value_or(CommandId{});
    REQUIRE(switch_buffer);
    CommandContext switch_context(application.runtime(), application.window_id(),
                                  application.buffer_id(), application.view_id());
    const CommandResult switched = application.runtime().commands().invoke(
        switch_buffer, switch_context,
        CommandInvocation{.arguments = {application.runtime().buffers().get(first).name()},
                          .prefix = {}});
    INFO((switched ? std::string{} : switched.error().message));
    REQUIRE(switched.has_value());
    CHECK(application.session().snapshot().content().to_string() == "Afirst");
    CHECK(application.session().caret().value == 1);
    CHECK(application.session().view().viewport().top_line_offset == doctest::Approx(0.25F));
    send_keys(application, "C-x Right");
    CHECK(application.session().snapshot().content().to_string() == "Bsecond");
    CHECK(application.session().view().viewport().left_column == 3);
    send_keys(application, "C-x Left");
    CHECK(application.buffer_id() == first);
    send_keys(application, "C-x Right");
    CHECK(application.buffer_id() == second);

    const std::expected<void, std::string> reused = application.open_file(second_path.string());
    REQUIRE(reused.has_value());
    CHECK(application.buffer_id() == second);
    CHECK(application.buffer_count() == 2);
    send_keys(application, "C-x k");
    CHECK(application.message() == "buffer has unsaved changes");
    CHECK(application.buffer_id() == second);
    const CommandId force_kill =
        application.runtime().commands().find("buffer.force-kill").value_or(CommandId{});
    REQUIRE(force_kill);
    CommandContext kill_context(application.runtime(), application.window_id(),
                                application.buffer_id(), application.view_id());
    const CommandResult killed = application.runtime().commands().invoke(force_kill, kill_context);
    REQUIRE(killed.has_value());
    CHECK(application.buffer_count() == 1);
    CHECK(application.buffer_id() == first);

    CHECK(application.kill_buffer(first, true).has_value());
    CHECK(application.buffer_count() == 1);
    CHECK(application.session().buffer().name() == "*scratch*");
    CHECK_FALSE(application.dirty());

    std::filesystem::remove_all(directory, ignored);
}
