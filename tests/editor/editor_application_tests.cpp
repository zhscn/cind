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
#include <variant>

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
                              .platform_services = std::move(platform_services),
                              .init_file = std::nullopt});
}

void send_keys(EditorApplication& application, std::string_view notation) {
    const std::expected<KeySequence, KeyParseError> sequence = parse_key_sequence(notation);
    REQUIRE(sequence.has_value());
    for (const KeyStroke key : *sequence) {
        CHECK(application.handle_key(key, 10));
    }
}

CommandId require_command(const EditorRuntime& runtime, std::string_view name) {
    const std::optional<CommandId> command = runtime.commands().find(name);
    if (!command) {
        FAIL("missing command: ", name);
        return {};
    }
    return *command;
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

    CHECK(application.session().snapshot().content() == "abyxcyd");
    const ViewSelection expected{.ranges = {{.anchor = TextOffset{0},
                                             .head = TextOffset{3},
                                             .granularity = SelectionGranularity::Character},
                                            {.anchor = TextOffset{4},
                                             .head = TextOffset{6},
                                             .granularity = SelectionGranularity::Node}},
                                 .primary = 0,
                                 .metadata = "(strategy . preserve)"};
    CHECK(application.session().active_selection() == expected);
    CHECK(application.session().undo());
    CHECK(application.session().snapshot().content() == "abxcd");
    CHECK(application.session().redo());
    CHECK(application.session().snapshot().content() == "abyxcyd");
}

TEST_CASE("typed character input reindents every selection head in one transaction") {
    EditorApplication application =
        make_application("sample.cc", "switch (x) {\n    case 1\n    case 2\n}\n");
    EditorRuntime& runtime = application.runtime();
    const InputStateId emacs = runtime.input_states().find("emacs").value_or(InputStateId{});
    REQUIRE(emacs);
    const InputStrategyId preserve =
        runtime.input_strategies().define({.name = "test-multi-input",
                                           .editing = emacs,
                                           .interface = emacs,
                                           .selection_after_edit = SelectionEditPolicy::Preserve});
    runtime.views().set_input_strategy(application.view_id(), preserve);
    const std::string before = application.session().snapshot().content().to_string();
    const TextOffset first{static_cast<std::uint32_t>(before.find("case 1") + 6)};
    const TextOffset second{static_cast<std::uint32_t>(before.find("case 2") + 6)};
    runtime.views().set_selection(application.view_id(),
                                  ViewSelection{.ranges = {{.anchor = first, .head = first},
                                                           {.anchor = second, .head = second}},
                                                .primary = 1,
                                                .metadata = "(input . multi)"});

    application.insert_text(":");

    CHECK(application.session().snapshot().content() == "switch (x) {\ncase 1:\ncase 2:\n}\n");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.size() == 2);
    CHECK(application.session().undo());
    CHECK(application.session().snapshot().content() == before);
    CHECK_FALSE(application.session().undo());
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
    CommandInvocation handled_invocation;
    const CommandId handled_command = runtime.commands().define(
        "test.state.handler",
        [&](CommandContext&, const CommandInvocation& invocation) -> CommandResult {
            selected = 3;
            handled_invocation = invocation;
            return CommandCompleted{};
        });
    WindowId handler_window;
    const KeymapId base_map = runtime.keymaps().define("test.state.base-map");
    const KeymapId transient_map = runtime.keymaps().define("test.state.transient-map");
    runtime.keymaps().bind(base_map, "z", base_command);
    runtime.keymaps().bind(transient_map, "z", transient_command);
    int position_hint_calls = 0;
    const InputStateId base = runtime.input_states().define(
        {.name = "test-base",
         .keymaps = {base_map},
         .indicator = "B",
         .handler = {},
         .position_hints = [&](CommandContext& context) -> PositionHintProviderResult {
             ++position_hint_calls;
             CHECK(context.view_id() == application.view_id());
             return std::vector<PositionHint>{{.position = TextOffset{1}, .label = "1"}};
         },
         .on_enter = {},
         .on_exit = {}});
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
                                                .invocation = {},
                                                .feedback = std::nullopt};
             }
             if (format_key_stroke(key) == "d") {
                 return InputStateHandlerAction{
                     .kind = InputStateHandlerActionKind::Dispatch,
                     .command = handled_command,
                     .invocation = {.arguments = {std::string("captured")}, .prefix = {}},
                     .feedback = std::nullopt};
             }
             if (format_key_stroke(key) == "p") {
                 return InputStateHandlerAction{
                     .kind = InputStateHandlerActionKind::Pending,
                     .command = {},
                     .invocation = {},
                     .feedback = InputFeedback{
                         .sequence = "C-x",
                         .hints = {{.key = "C-s", .detail = "file.save", .prefix = false},
                                   {.key = "4", .detail = "window", .prefix = true}}}};
             }
             return InputStateHandlerAction{};
         },
         .position_hints = {},
         .on_enter = {},
         .on_exit = {}});
    runtime.views().set_base_input_state(application.view_id(), base);
    runtime.views().push_input_state(application.view_id(), transient);
    REQUIRE(application.position_hints(application.window_id()).has_value());
    CHECK(application.position_hints(application.window_id())->empty());
    CHECK(position_hint_calls == 0);

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
    application.command_loop().set_pending_prefix(
        {.count = 7, .register_name = std::string("a"), .extra = {}});
    CHECK(application.handle_key(KeyStroke::character_key(U'd'), 10));
    CHECK(selected == 3);
    REQUIRE(handled_invocation.arguments.size() == 1);
    CHECK(std::get<std::string>(handled_invocation.arguments.front()) == "captured");
    CHECK(handled_invocation.prefix.count == 7);
    CHECK(handled_invocation.prefix.register_name == std::optional<std::string>{"a"});
    CHECK(application.command_loop().pending_prefix().empty());
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
    const PositionHintProviderResult first_hints =
        application.position_hints(application.window_id());
    REQUIRE(first_hints.has_value());
    CHECK(*first_hints == std::vector<PositionHint>{{.position = TextOffset{1}, .label = "1"}});
    CHECK(position_hint_calls == 1);
    CHECK(application.position_hints(application.window_id()) == first_hints);
    CHECK(position_hint_calls == 1);

    application.session().set_caret(TextOffset{2});
    REQUIRE(application.position_hints(application.window_id()).has_value());
    CHECK(position_hint_calls == 2);
    application.insert_text("!");
    REQUIRE(application.position_hints(application.window_id()).has_value());
    CHECK(position_hint_calls == 3);
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
                                       .handler = {},
                                       .position_hints = {},
                                       .on_enter = {},
                                       .on_exit = {}});
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

    send_keys(application, "SPC 1 2");
    CHECK(application.input_state().name == "meow-numeric");
    CHECK(application.pending_prefix_text() == "12");
    CHECK(application.pending_command_text() == "12");
    CHECK(application.command_loop().pending_prefix().count == 12);
    send_keys(application, "l");
    CHECK(application.session().caret() == TextOffset{12});
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_prefix_text().empty());

    application.session().set_caret(TextOffset{0});
    send_keys(application, "SPC 3");
    send_keys(application, "\"");
    CHECK(application.input_state().name == "input.read-key");
    CHECK(application.pending_command_text() == "3 \"");
    send_keys(application, "a");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_command_text() == "3 \"a");
    CHECK(application.command_loop().pending_prefix().register_name ==
          std::optional<std::string>{"a"});
    send_keys(application, "l");
    CHECK(application.session().caret() == TextOffset{3});
    CHECK(application.pending_command_text().empty());

    send_keys(application, "- 2 l");
    CHECK(application.session().caret() == TextOffset{1});
    CHECK(application.input_state().name == "meow-normal");

    send_keys(application, "\"");
    CHECK(application.input_state().name == "input.read-key");
    send_keys(application, "C-g");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_command_text().empty());

    send_keys(application, "\" b");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.command_loop().pending_prefix().register_name ==
          std::optional<std::string>{"b"});
    send_keys(application, "l");
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
    EditorApplication application = make_application(
        "sample.cc", "one two three vector<int> four five six seven eight nine ten eleven");
    EditorRuntime& runtime = application.runtime();
    send_keys(application, "C-c m");

    send_keys(application, "SPC 3 w");
    CHECK(application.session().caret() == TextOffset{14});
    CHECK(application.pending_command_text().empty());
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(0, 14));
    CHECK(application.session().active_selection()->metadata.find("strategy . meow") !=
          std::string::npos);

    application.session().clear_selection();
    application.session().set_caret(TextOffset{0});
    send_keys(application, "w");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(0, 4));
    CHECK(application.session().active_selection()->metadata.find("type select . word") !=
          std::string::npos);
    const PositionHintProviderResult first_hints =
        application.position_hints(application.window_id());
    REQUIRE(first_hints.has_value());
    REQUIRE(first_hints->size() == 10);
    CHECK(first_hints->front().label == "1");
    CHECK(first_hints->front().position.value > 4);
    CHECK((*first_hints)[1].label == "2");
    CHECK((*first_hints)[1].position > first_hints->front().position);
    CHECK(first_hints->back().label == "0");
    CHECK(application.position_hints(application.window_id()) == first_hints);
    send_keys(application, "2");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(0, 14));
    CHECK(application.session().active_selection()->metadata.find("type expand . word") !=
          std::string::npos);
    const PositionHintProviderResult expanded_hints =
        application.position_hints(application.window_id());
    REQUIRE(expanded_hints.has_value());
    REQUIRE_FALSE(expanded_hints->empty());
    CHECK(expanded_hints->front().position.value > 14);

    send_keys(application, "x");
    REQUIRE(application.position_hints(application.window_id()).has_value());
    CHECK(application.position_hints(application.window_id())->empty());
    send_keys(application, "C-g");
    REQUIRE(application.position_hints(application.window_id()).has_value());
    CHECK(application.position_hints(application.window_id())->empty());
    send_keys(application, "w");
    REQUIRE_FALSE(application.position_hints(application.window_id())->empty());

    application.session().clear_selection();
    application.session().set_caret(TextOffset{8});
    send_keys(application, "[");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(8, 13));
    CHECK(application.session().active_selection()->metadata.find("type expand . word") !=
          std::string::npos);

    application.session().set_caret(TextOffset{21});
    send_keys(application, ",");
    CHECK(application.input_state().name == "input.read-key");
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

    runtime.views().set_selection(
        application.view_id(),
        ViewSelection{.ranges = {{.anchor = TextOffset{1},
                                  .head = TextOffset{1},
                                  .granularity = SelectionGranularity::Character},
                                 {.anchor = TextOffset{5},
                                  .head = TextOffset{5},
                                  .granularity = SelectionGranularity::Character}},
                      .primary = 1,
                      .metadata = "((source . test))"});
    send_keys(application, ", w");
    REQUIRE(application.session().active_selection().has_value());
    const ViewSelection selected = *application.session().active_selection();
    REQUIRE(selected.ranges.size() == 2);
    CHECK(selected.ranges[0].ordered() == make_range(0, 3));
    CHECK(selected.ranges[1].ordered() == make_range(4, 7));
    CHECK(selected.primary == 1);
}

TEST_CASE("Guile Vim policy composes states prefixes operators and things") {
    std::string clipboard;
    EditorPlatformServices platform{
        .write_clipboard = [&](std::string_view text) -> std::expected<void, std::string> {
            clipboard = text;
            return {};
        },
        .read_clipboard = [&]() -> std::expected<std::string, std::string> { return clipboard; },
        .wake_event_loop = {}};
    EditorApplication application =
        make_application("sample.cc", "one <abc> two three", std::move(platform));

    send_keys(application, "C-c v");
    CHECK(application.input_state().name == "vim-normal");
    const std::optional<InputStrategyId> strategy =
        application.runtime().views().get(application.view_id()).input_strategy();
    REQUIRE(strategy.has_value());
    CHECK(application.runtime().input_strategies().definition(*strategy).name == "vim");

    send_keys(application, "2 w");
    CHECK(application.session().caret() == TextOffset{5});
    application.session().set_caret(TextOffset{0});
    application.runtime().views().clear_selection(application.view_id());

    send_keys(application, "d");
    CHECK(application.input_state().name == "vim-operator");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(0, 1));
    send_keys(application, "C-g");
    CHECK(application.input_state().name == "vim-normal");
    CHECK_FALSE(application.session().active_selection().has_value());

    send_keys(application, "d \" a w");
    CHECK(application.input_state().name == "vim-normal");
    CHECK(application.session().snapshot().content() == "<abc> two three");
    REQUIRE(application.session().undo());
    application.session().set_caret(TextOffset{0});
    application.runtime().views().clear_selection(application.view_id());

    send_keys(application, "\" a d w");
    CHECK(application.session().snapshot().content() == "<abc> two three");
    CHECK(clipboard == "one ");
    application.session().set_caret(
        TextOffset{application.session().snapshot().content().size_bytes()});
    send_keys(application, "\" a p");
    CHECK(application.session().snapshot().content() == "<abc> two threeone ");

    REQUIRE(application.session().undo());
    REQUIRE(application.session().undo());
    application.session().set_caret(TextOffset{6});
    application.runtime().views().clear_selection(application.view_id());
    send_keys(application, "d i a");
    CHECK(application.session().snapshot().content() == "one <> two three");

    REQUIRE(application.session().undo());
    application.session().set_caret(TextOffset{0});
    application.runtime().views().clear_selection(application.view_id());
    send_keys(application, "v w");
    CHECK(application.input_state().name == "vim-visual");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().ordered() == make_range(0, 4));
    send_keys(application, "d");
    CHECK(application.input_state().name == "vim-normal");
    CHECK(application.session().snapshot().content() == "<abc> two three");

    send_keys(application, "i");
    CHECK(application.input_state().name == "vim-insert");
    application.insert_text("X");
    send_keys(application, "ESC");
    CHECK(application.input_state().name == "vim-normal");
}

TEST_CASE("Guile Helix policy transforms every selection through shared nouns") {
    EditorApplication application = make_application("sample.cc", "one two three four");
    EditorRuntime& runtime = application.runtime();

    send_keys(application, "C-c h");
    CHECK(application.input_state().name == "hx-normal");
    const std::optional<InputStrategyId> strategy =
        runtime.views().get(application.view_id()).input_strategy();
    REQUIRE(strategy.has_value());
    CHECK(runtime.input_strategies().definition(*strategy).name == "helix");

    const auto set_carets = [&](TextOffset first, TextOffset second) {
        runtime.views().set_selection(
            application.view_id(),
            ViewSelection{.ranges = {{.anchor = first,
                                      .head = first,
                                      .granularity = SelectionGranularity::Character},
                                     {.anchor = second,
                                      .head = second,
                                      .granularity = SelectionGranularity::Character}},
                          .primary = 1,
                          .metadata = "((strategy . helix))"});
    };

    set_carets(TextOffset{0}, TextOffset{8});
    send_keys(application, "w");
    REQUIRE(application.session().active_selection().has_value());
    ViewSelection selected = *application.session().active_selection();
    REQUIRE(selected.ranges.size() == 2);
    CHECK(selected.ranges[0].head == TextOffset{4});
    CHECK(selected.ranges[1].head == TextOffset{14});
    CHECK(selected.primary == 1);

    set_carets(TextOffset{1}, TextOffset{5});
    send_keys(application, "m i");
    CHECK(application.input_state().name == "input.read-key");
    CHECK(application.pending_key_sequence_text() == "m i");
    send_keys(application, "w");
    CHECK(application.input_state().name == "hx-normal");
    REQUIRE(application.session().active_selection().has_value());
    selected = *application.session().active_selection();
    REQUIRE(selected.ranges.size() == 2);
    CHECK(selected.ranges[0].ordered() == make_range(0, 3));
    CHECK(selected.ranges[1].ordered() == make_range(4, 7));
    CHECK(selected.primary == 1);

    send_keys(application, "d");
    CHECK(application.input_state().name == "hx-normal");
    CHECK(application.session().snapshot().content() == "  three four");
    REQUIRE(application.session().undo());
    CHECK(application.session().snapshot().content() == "one two three four");

    set_carets(TextOffset{0}, TextOffset{8});
    send_keys(application, "v w");
    CHECK(application.input_state().name == "hx-select");
    REQUIRE(application.session().active_selection().has_value());
    selected = *application.session().active_selection();
    CHECK(selected.ranges[0].anchor == TextOffset{0});
    CHECK(selected.ranges[0].head == TextOffset{4});
    CHECK(selected.ranges[1].anchor == TextOffset{8});
    CHECK(selected.ranges[1].head == TextOffset{14});
    CHECK(selected.primary == 1);

    send_keys(application, "ESC i");
    CHECK(application.input_state().name == "hx-insert");
    application.insert_text("X");
    send_keys(application, "ESC");
    CHECK(application.input_state().name == "hx-normal");
}

TEST_CASE("Guile structural state expands and consumes multi-range nodes") {
    const std::string source = "int f() { int alpha; int beta; }\n";
    EditorApplication application = make_application("sample.cc", source);
    EditorRuntime& runtime = application.runtime();
    const TextOffset alpha{static_cast<std::uint32_t>(source.find("alpha") + 2)};
    const TextOffset beta{static_cast<std::uint32_t>(source.find("beta") + 2)};
    runtime.views().set_selection(
        application.view_id(),
        ViewSelection{
            .ranges =
                {{.anchor = alpha, .head = alpha, .granularity = SelectionGranularity::Character},
                 {.anchor = beta, .head = beta, .granularity = SelectionGranularity::Character}},
            .primary = 1,
            .metadata = "((origin . structural-test))"});

    send_keys(application, "C-c e");
    CHECK(application.input_state().name == "structural-node");
    CHECK(runtime.views().selection_history_size(application.view_id()) == 1);
    REQUIRE(application.session().active_selection().has_value());
    const ViewSelection identifiers = *application.session().active_selection();
    REQUIRE(identifiers.ranges.size() == 2);
    CHECK(identifiers.ranges[0].ordered() ==
          make_range(static_cast<std::uint32_t>(source.find("alpha")),
                     static_cast<std::uint32_t>(source.find("alpha") + 5)));
    CHECK(identifiers.ranges[1].ordered() ==
          make_range(static_cast<std::uint32_t>(source.find("beta")),
                     static_cast<std::uint32_t>(source.find("beta") + 4)));
    CHECK(identifiers.ranges[0].granularity == SelectionGranularity::Node);
    CHECK(identifiers.ranges[1].granularity == SelectionGranularity::Node);
    CHECK(identifiers.primary == 1);
    CHECK(identifiers.metadata == "((origin . structural-test))");

    send_keys(application, "e");
    CHECK(runtime.views().selection_history_size(application.view_id()) == 2);
    REQUIRE(application.session().active_selection().has_value());
    const ViewSelection declarations = *application.session().active_selection();
    REQUIRE(declarations.ranges.size() == 2);
    CHECK(declarations.ranges[0].ordered().start <= identifiers.ranges[0].ordered().start);
    CHECK(declarations.ranges[0].ordered().end >= identifiers.ranges[0].ordered().end);
    CHECK(declarations.ranges[1].ordered().start <= identifiers.ranges[1].ordered().start);
    CHECK(declarations.ranges[1].ordered().end >= identifiers.ranges[1].ordered().end);
    CHECK(declarations.ranges[0].ordered() != identifiers.ranges[0].ordered());
    CHECK(declarations.ranges[1].ordered() != identifiers.ranges[1].ordered());

    send_keys(application, "h");
    CHECK(runtime.views().selection_history_size(application.view_id()) == 1);
    REQUIRE(application.session().active_selection().has_value());
    CHECK(*application.session().active_selection() == identifiers);

    send_keys(application, "d");
    CHECK(application.input_state().name == "emacs");
    CHECK(runtime.views().selection_history_size(application.view_id()) == 0);
    CHECK(application.session().snapshot().content() == "int f() { int ; int ; }\n");
    REQUIRE(application.session().undo());
    CHECK(application.session().snapshot().content() == source);

    runtime.views().set_selection(
        application.view_id(),
        {.anchor = alpha, .head = alpha, .granularity = SelectionGranularity::Character});
    send_keys(application, "C-c e ESC");
    CHECK(application.input_state().name == "emacs");
    CHECK(runtime.views().selection_history_size(application.view_id()) == 0);
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().granularity ==
          SelectionGranularity::Node);

    const TextOffset alpha_start{static_cast<std::uint32_t>(source.find("alpha"))};
    const TextOffset alpha_end{alpha_start.value + 5};
    runtime.views().set_selection(
        application.view_id(),
        {.anchor = alpha_end, .head = alpha_start, .granularity = SelectionGranularity::Node});
    send_keys(application, "C-c e");
    REQUIRE(application.session().active_selection().has_value());
    CHECK(application.session().active_selection()->ranges.front().anchor >
          application.session().active_selection()->ranges.front().head);
    send_keys(application, "ESC");
    CHECK(runtime.views().selection_history_size(application.view_id()) == 0);

    const ViewSelection incomplete{
        .ranges = {{.anchor = alpha, .head = alpha, .granularity = SelectionGranularity::Character},
                   {.anchor = TextOffset{0},
                    .head = TextOffset{static_cast<std::uint32_t>(source.size())},
                    .granularity = SelectionGranularity::Node}},
        .primary = 0,
        .metadata = "((all-or-none . test))"};
    runtime.views().set_selection(application.view_id(), incomplete);
    send_keys(application, "C-c e");
    CHECK(application.input_state().name == "emacs");
    CHECK(runtime.views().selection_history_size(application.view_id()) == 0);
    CHECK(application.session().active_selection() == incomplete);
    CHECK(application.message() == "no enclosing syntax node at every selection");
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

TEST_CASE("region commands preserve per-range kill entries and edit atomically") {
    std::string clipboard;
    EditorApplication application = make_application(
        "sample.cc", "one\ntwo\nthree\n",
        {.write_clipboard = [&](std::string_view text) -> std::expected<void, std::string> {
             clipboard = text;
             return {};
         },
         .read_clipboard = {},
         .wake_event_loop = {}});
    EditorRuntime& runtime = application.runtime();
    CommandContext context(runtime, application.window_id(), application.buffer_id(),
                           application.view_id());
    const ViewSelection regions{.ranges = {{.anchor = TextOffset{0},
                                            .head = TextOffset{3},
                                            .granularity = SelectionGranularity::Character},
                                           {.anchor = TextOffset{4},
                                            .head = TextOffset{7},
                                            .granularity = SelectionGranularity::Line}},
                                .primary = 0,
                                .metadata = "(region . multi)"};

    runtime.views().set_selection(application.view_id(), regions);
    application.command_loop().set_pending_prefix(
        {.count = std::nullopt, .register_name = "a", .extra = {}});
    CHECK(application.command_loop()
              .execute(require_command(runtime, "edit.copy-region"), context)
              .status == CommandLoopStatus::Executed);
    CHECK(clipboard == "one\ntwo\n");
    CHECK_FALSE(application.session().active_selection().has_value());

    runtime.views().set_selection(
        application.view_id(),
        ViewSelection{.ranges = {{.anchor = TextOffset{0}, .head = TextOffset{0}},
                                 {.anchor = TextOffset{14}, .head = TextOffset{14}}},
                      .primary = 1,
                      .metadata = "(yank . multi)"});
    application.command_loop().set_pending_prefix(
        {.count = std::nullopt, .register_name = "a", .extra = {}});
    CHECK(
        application.command_loop().execute(require_command(runtime, "edit.yank"), context).status ==
        CommandLoopStatus::Executed);
    CHECK(application.session().snapshot().content() == "oneone\ntwo\nthree\ntwo\n");
    CHECK(application.session().undo());
    CHECK(application.session().snapshot().content() == "one\ntwo\nthree\n");
    CHECK_FALSE(application.session().undo());

    runtime.views().set_selection(application.view_id(), regions);
    CHECK(application.command_loop()
              .execute(require_command(runtime, "edit.kill-region"), context)
              .status == CommandLoopStatus::Executed);
    CHECK(application.session().snapshot().content() == "\nthree\n");
    CHECK(clipboard == "one\ntwo\n");
    CHECK(application.session().undo());
    CHECK(application.session().snapshot().content() == "one\ntwo\nthree\n");
    CHECK_FALSE(application.session().undo());
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
        .init_file = std::nullopt,
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
            .init_file = std::nullopt,
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

TEST_CASE("Emacs universal argument composes through a transient input state") {
    EditorApplication application =
        make_application("sample.cc", "abcdefghijklmnopqrstuvwxyz0123456789");

    send_keys(application, "C-u");
    CHECK(application.input_state().name == "emacs-universal");
    CHECK(application.pending_prefix_text() == "4");
    send_keys(application, "C-f");
    CHECK(application.session().caret().value == 4);
    CHECK(application.input_state().name == "emacs");
    CHECK(application.pending_prefix_text().empty());

    send_keys(application, "C-u C-u");
    CHECK(application.input_state().name == "emacs-universal");
    CHECK(application.pending_prefix_text() == "16");
    CHECK(application.message() == "");
    send_keys(application, "C-f");
    CHECK(application.session().caret().value == 20);

    application.session().set_caret(TextOffset{0});
    send_keys(application, "C-u 1 2");
    CHECK(application.pending_prefix_text() == "12");
    CHECK(application.message() == "");
    send_keys(application, "C-f");
    CHECK(application.session().caret().value == 12);

    send_keys(application, "C-u - 2 C-f");
    CHECK(application.session().caret().value == 10);

    send_keys(application, "C-u 3 C-g");
    CHECK(application.input_state().name == "emacs");
    CHECK(application.pending_prefix_text().empty());
    CHECK(application.session().caret().value == 10);
    send_keys(application, "C-u");
    CHECK(application.input_state().name == "emacs-universal");
    send_keys(application, "C-g");

    EditorApplication insertion = make_application("sample.cc", "");
    send_keys(insertion, "C-u 3");
    CHECK_FALSE(insertion.handle_key(KeyStroke::character_key(U'x'), 10));
    CHECK(insertion.pending_prefix_text() == "3");
    insertion.insert_text("x");
    CHECK(insertion.session().snapshot().content().to_string() == "xxx");
    CHECK(insertion.pending_prefix_text().empty());

    send_keys(insertion, "C-u -");
    CHECK_FALSE(insertion.handle_key(KeyStroke::character_key(U'y'), 10));
    insertion.insert_text("y");
    CHECK(insertion.session().snapshot().content().to_string() == "xxx");
    CHECK(insertion.message() == "negative repeat count is invalid for text input");
    CHECK(insertion.pending_prefix_text().empty());
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

TEST_CASE("view release runs state lifecycle while its editor session is available") {
    EditorApplication application = make_application("sample.cc", "one two\n");
    REQUIRE(application.split_window(WindowSplitAxis::Columns));
    const WindowId transient_window = application.window_layout().leaves().back();
    REQUIRE(application.focus_window(transient_window));

    send_keys(application, "C-c v d");
    CHECK(application.input_state().name == "vim-operator");
    REQUIRE(application.session().active_selection().has_value());

    REQUIRE(application.delete_window());
    CHECK(application.open_windows().size() == 1);
    CHECK(application.runtime().windows().try_get(transient_window) == nullptr);
    CHECK_FALSE(application.scripting().last_error.has_value());
}

TEST_CASE("deleting the sole window preserves the application focus target") {
    EditorApplication application = make_application("sample.cc", "text");
    const WindowId window = application.window_id();

    send_keys(application, "C-x 0");
    CHECK(application.window_id() == window);
    CHECK(application.open_windows().size() == 1);
    CHECK(application.message() == "cannot delete the only window");
}

TEST_CASE("describe bindings and command palette use shared command state") {
    EditorApplication application = make_application("sample.cc", "text");

    send_keys(application, "C-h b");
    CHECK(application.session().buffer().name() == "*Help*");
    const std::string help = application.session().snapshot().content().to_string();
    CHECK(help.find("C-x C-s  file.save") != std::string::npos);
    CHECK(help.find("C-x C-c  application.quit") != std::string::npos);

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

TEST_CASE("describe commands display reusable generated help buffers") {
    EditorApplication application = make_application({}, "sample\n");
    EditorRuntime& runtime = application.runtime();
    const CommandId describe =
        runtime.commands().find("help.describe-bindings").value_or(CommandId{});
    REQUIRE(describe);
    const BufferId source = application.buffer_id();
    CommandContext context(runtime, application.window_id(), source, application.view_id());

    const CommandResult first = runtime.commands().invoke(describe, context);
    REQUIRE(first.has_value());
    const BufferId help = application.buffer_id();
    const Buffer& help_buffer = runtime.buffers().get(help);
    CHECK(help_buffer.name() == "*Help*");
    CHECK(help_buffer.kind() == BufferKind::Generated);
    CHECK(help_buffer.read_only());
    const ModeId help_mode = help_buffer.modes().major().value_or(ModeId{});
    REQUIRE(help_mode);
    CHECK(runtime.modes().definition(help_mode).name == "special-mode");
    CHECK(help_buffer.snapshot().content().to_string().find("Active key bindings") !=
          std::string::npos);

    REQUIRE(application.switch_buffer(source));
    CommandContext repeated_context(runtime, application.window_id(), application.buffer_id(),
                                    application.view_id());
    const CommandResult second = runtime.commands().invoke(describe, repeated_context);
    REQUIRE(second.has_value());
    CHECK(application.buffer_id() == help);
    CHECK(application.buffer_count() == 2);
    CHECK(runtime.views().caret(application.view_id()) == TextOffset{});

    REQUIRE(application.switch_buffer(source));
    send_keys(application, "C-h k C-x C-s");
    CHECK(application.buffer_id() == help);
    const std::string key_help = application.session().snapshot().content().to_string();
    CHECK(key_help.find("file.save is a command") != std::string::npos);
    CHECK(key_help.find("Key sequence: C-x C-s") != std::string::npos);
}
