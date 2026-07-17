#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/command_loop.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/interaction.hpp"
#include "editor/location_list_mode.hpp"
#include "editor/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

} // namespace

TEST_CASE("settings are declared, typed, scoped, and explicitly resolved") {
    EditorRuntime runtime;
    const SettingId tab_width = runtime.setting_definitions().define(
        "editor.tab-width", std::int64_t{4},
        SettingScope::Application | SettingScope::Project | SettingScope::Mode |
            SettingScope::Buffer | SettingScope::View);
    const SettingId internal = runtime.setting_definitions().define(
        "editor.internal", false, setting_scope_bit(SettingScope::Application));

    const ModeId text_mode = runtime.modes().define("text", ModeKind::Major);
    runtime.modes().definition_for_configuration(text_mode).defaults.set(tab_width,
                                                                         std::int64_t{2});
    const BufferId buffer = runtime.buffers().create({.name = "notes",
                                                      .initial_text = "hello",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), text_mode);
    const ViewId view = runtime.views().create(buffer);

    CHECK(runtime.settings_for(buffer, view).get_as<std::int64_t>(tab_width) == 2);
    runtime.application_settings().set(tab_width, std::int64_t{8});
    CHECK(runtime.settings_for(buffer, view).get_as<std::int64_t>(tab_width) == 8);
    runtime.buffers().get(buffer).settings().set(tab_width, std::int64_t{3});
    CHECK(runtime.settings_for(buffer, view).get_as<std::int64_t>(tab_width) == 3);
    runtime.views().get(view).settings().set(tab_width, std::int64_t{6});
    CHECK(runtime.settings_for(buffer, view).get_as<std::int64_t>(tab_width) == 6);

    CHECK_THROWS_AS(runtime.views().get(view).settings().set(internal, true),
                    std::invalid_argument);
    CHECK_THROWS_AS(runtime.buffers().get(buffer).settings().set(tab_width, true),
                    std::invalid_argument);

    runtime.seal_extensions();
    CHECK_THROWS_AS(runtime.setting_definitions().define("late.value", false), std::logic_error);
    CHECK_THROWS_AS(runtime.modes().define("late", ModeKind::Minor), std::logic_error);
    CHECK_THROWS_AS(runtime.modes().definition_for_configuration(text_mode), std::logic_error);

    // Explicit runtime preference layers remain configurable after schemas
    // and extension definitions are frozen.
    runtime.application_settings().set(tab_width, std::int64_t{10});
    CHECK(runtime.application_settings().find(tab_width) != nullptr);
}

TEST_CASE("buffers expose validated semantic source locations") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "locations",
                                                      .initial_text = "first\nsecond\n",
                                                      .kind = BufferKind::Generated,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = true});
    runtime.buffers().set_locations(buffer, {{.source_range = make_range(0, 6),
                                              .resource = "/work/first.cpp",
                                              .target = {.line = 4, .byte_column = 2}},
                                             {.source_range = make_range(6, 13),
                                              .resource = "/work/second.cpp",
                                              .target = {.line = 8, .byte_column = 1}}});

    const Buffer& result = runtime.buffers().get(buffer);
    REQUIRE(result.location_at(TextOffset{2}) != nullptr);
    CHECK(result.location_at(TextOffset{2})->resource == "/work/first.cpp");
    REQUIRE(result.location_at(result.snapshot().content().end_offset()) != nullptr);
    CHECK_THROWS_AS(
        runtime.buffers().set_locations(
            buffer, {{.source_range = make_range(0, 7), .resource = "/work/a.cpp", .target = {}},
                     {.source_range = make_range(6, 8), .resource = "/work/b.cpp", .target = {}}}),
        std::invalid_argument);
    Buffer& mutable_result = runtime.buffers().get(buffer);
    mutable_result.set_read_only(false);
    auto transaction = mutable_result.begin_transaction();
    transaction.insert(mutable_result.snapshot().content().end_offset(), "changed");
    transaction.commit();
    CHECK(mutable_result.locations().empty());
}

TEST_CASE("location-list mode owns sparse navigation bindings without a language profile") {
    EditorRuntime runtime;
    const auto command = [&](std::string name) {
        return runtime.commands().define(
            std::move(name), [](CommandContext&, const CommandInvocation&) -> CommandResult {
                return CommandCompleted{};
            });
    };
    const CommandId visit = command("location.visit");
    const CommandId next = command("location.next");
    const CommandId previous = command("location.previous");
    const LocationListModeRegistration mode =
        ensure_location_list_mode(runtime, {.visit = visit, .next = next, .previous = previous});

    CHECK_FALSE(runtime.modes().definition(mode.mode).language.has_value());
    CHECK(runtime.modes().definition(mode.mode).keymaps == std::vector<KeymapId>{mode.keymap});
    const KeySequence enter = *parse_key_sequence("RET");
    const KeySequence alt_next = *parse_key_sequence("M-n");
    CHECK(runtime.keymaps().resolve(mode.keymap, enter).command == visit);
    CHECK(runtime.keymaps().resolve(mode.keymap, alt_next).command == next);
    CHECK(ensure_location_list_mode(runtime, {.visit = visit, .next = next, .previous = previous})
              .mode == mode.mode);
}

TEST_CASE("major modes select composed language profiles instead of inheriting parsers") {
    EditorRuntime runtime;
    const SettingId dialect = runtime.setting_definitions().define(
        "language.c-family.dialect", std::string("c++"),
        SettingScope::Language | SettingScope::Project | SettingScope::Buffer);

    const LanguageProviderId lexer =
        runtime.languages().define_provider("cind.c-family.lexer", LanguageFacet::Lexing);
    const LanguageProviderId syntax =
        runtime.languages().define_provider("cind.c-family.syntax", LanguageFacet::Syntax);
    const LanguageProviderId indentation = runtime.languages().define_provider(
        "cind.c-family.indentation", LanguageFacet::Indentation);
    const LanguageProviderId editing = runtime.languages().define_provider(
        "cind.c-family.structural-editing", LanguageFacet::StructuralEditing);

    const LanguageProfileId cpp = runtime.languages().define_profile("cpp");
    for (const auto [facet, provider] :
         {std::pair{LanguageFacet::Lexing, lexer}, std::pair{LanguageFacet::Syntax, syntax},
          std::pair{LanguageFacet::Indentation, indentation},
          std::pair{LanguageFacet::StructuralEditing, editing}}) {
        runtime.languages().bind(cpp, facet, provider);
    }
    runtime.languages().profile_for_configuration(cpp).defaults.set(dialect, std::string("c++"));

    const LanguageProfileId c = runtime.languages().define_profile("c");
    runtime.languages().bind(c, LanguageFacet::Lexing, lexer);
    runtime.languages().bind(c, LanguageFacet::Syntax, syntax);
    runtime.languages().bind(c, LanguageFacet::Indentation, indentation);
    runtime.languages().bind(c, LanguageFacet::StructuralEditing, editing);
    runtime.languages().profile_for_configuration(c).defaults.set(dialect, std::string("c"));

    const ModeId cpp_mode = runtime.modes().define("cpp", ModeKind::Major, cpp);
    CHECK_THROWS_AS(runtime.modes().define("bad-minor", ModeKind::Minor, cpp),
                    std::invalid_argument);
    CHECK_THROWS_AS(runtime.languages().bind(cpp, LanguageFacet::Formatting, syntax),
                    std::invalid_argument);

    const BufferId buffer = runtime.buffers().create({.name = "main.cpp",
                                                      .initial_text = "",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), cpp_mode);
    const ViewId view = runtime.views().create(buffer);
    CHECK(runtime.settings_for(buffer, view).get_as<std::string>(dialect) == "c++");
    CHECK(runtime.languages().profile(cpp).provider(LanguageFacet::Syntax) == syntax);
    CHECK(runtime.languages().profile(c).provider(LanguageFacet::Syntax) == syntax);
}

TEST_CASE("the built-in C++ mode advertises native C-family editing facets") {
    EditorRuntime runtime;
    const CppModeRegistration cpp = ensure_cpp_mode(runtime);
    const LanguageRegistry::ProfileDefinition& profile = runtime.languages().profile(cpp.language);

    CHECK(profile.provider(LanguageFacet::Lexing).has_value());
    CHECK(profile.provider(LanguageFacet::Syntax).has_value());
    CHECK(profile.provider(LanguageFacet::Indentation).has_value());
    CHECK(profile.provider(LanguageFacet::StructuralEditing).has_value());
    CHECK(runtime.modes().definition(cpp.mode).language == cpp.language);
    CHECK(runtime.modes().definition(cpp.mode).things ==
          std::vector<ModeThingBinding>{{.name = "defun", .kind = "cst"},
                                        {.name = "string", .kind = "cst"}});
    CHECK(ensure_cpp_mode(runtime).mode == cpp.mode);
}

TEST_CASE("mode policy inheritance rederives every view input state") {
    EditorRuntime runtime;
    const auto input_state = [&](std::string name) {
        return runtime.input_states().define({.name = std::move(name),
                                              .keymaps = {},
                                              .text_input = TextInputPolicy::Ignore,
                                              .cursor = CursorShape::Block,
                                              .indicator = {},
                                              .handler = {}});
    };
    const InputStateId normal = input_state("normal");
    const InputStateId motion = input_state("motion");
    const InputStateId transient = input_state("transient");
    const InputStrategyId modal = runtime.input_strategies().define(
        {.name = "modal", .editing = normal, .interface = motion});
    const InputStrategyId alternate = runtime.input_strategies().define(
        {.name = "alternate", .editing = normal, .interface = transient});
    runtime.set_default_input_strategy(modal);

    const CommandId parent_command = runtime.commands().define(
        "test.parent", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    const KeymapId parent_map = runtime.keymaps().define("test.parent.map");
    runtime.keymaps().bind(parent_map, "p", parent_command);
    const ModeId fundamental = runtime.modes().define("fundamental", ModeKind::Major);
    runtime.modes().set_interaction_class(fundamental, InteractionClass::Editing);
    runtime.modes().set_things(fundamental, {{.name = "word", .kind = "character"}});
    runtime.modes().add_keymap(fundamental, parent_map);

    const KeymapId child_map = runtime.keymaps().define("test.child.map");
    const ModeId special = runtime.modes().define("special", ModeKind::Major);
    runtime.modes().set_parent(special, fundamental);
    runtime.modes().set_interaction_class(special, InteractionClass::Interface);
    runtime.modes().set_things(
        special, {{.name = "word", .kind = "interface"}, {.name = "item", .kind = "line"}});
    runtime.modes().add_keymap(special, child_map);
    CHECK(runtime.keymaps().parent(child_map) == parent_map);
    CHECK(runtime.modes().effective_keymaps(special) == std::vector<KeymapId>{child_map});
    CHECK(runtime.keymaps().resolve(child_map, *parse_key_sequence("p")).command == parent_command);

    const ModeId editable = runtime.modes().define("editable", ModeKind::Minor);
    runtime.modes().set_interaction_class(editable, InteractionClass::Editing);
    const ModeId forced = runtime.modes().define("forced", ModeKind::Minor);
    runtime.modes().set_initial_state(forced, transient);
    CHECK_THROWS_AS(runtime.modes().set_parent(editable, special), std::invalid_argument);
    CHECK_THROWS_AS(runtime.modes().set_parent(fundamental, special), std::invalid_argument);

    const BufferId buffer = runtime.buffers().create({.name = "interface",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Generated,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = true});
    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), special);
    const EffectiveModePolicy initial =
        runtime.modes().effective_policy(runtime.buffers().get(buffer).modes());
    CHECK(initial.interaction_class == InteractionClass::Interface);
    CHECK_FALSE(initial.initial_state);
    CHECK(initial.things == std::vector<ModeThingBinding>{{.name = "word", .kind = "interface"},
                                                          {.name = "item", .kind = "line"}});

    const ViewId first = runtime.views().create(buffer);
    const ViewId second = runtime.views().create(buffer);
    CHECK(runtime.views().get(first).input_states().base() == motion);
    CHECK(runtime.views().get(second).input_states().base() == motion);
    runtime.views().set_input_strategy(first, alternate);
    CHECK(runtime.views().get(first).input_states().base() == transient);
    CHECK(runtime.views().get(first).input_strategy() == alternate);
    CHECK_FALSE(runtime.views().get(second).input_strategy());

    std::vector<BufferModePolicyChange> changes;
    const ModeRegistry::ListenerId listener = runtime.modes().subscribe(
        [&](const BufferModePolicyChange& change) { changes.push_back(change); });
    REQUIRE(runtime.buffers().get(buffer).modes().enable_minor(runtime.modes(), editable));
    REQUIRE(changes.size() == 1);
    CHECK(changes.back().before.interaction_class == InteractionClass::Interface);
    CHECK(changes.back().after.interaction_class == InteractionClass::Editing);
    CHECK(runtime.views().get(first).input_states().base() == normal);
    CHECK(runtime.views().get(second).input_states().base() == normal);

    runtime.views().push_input_state(first, transient);
    REQUIRE(runtime.buffers().get(buffer).modes().disable_minor(editable));
    CHECK(runtime.views().get(first).input_states().base() == transient);
    CHECK(runtime.views().get(first).input_states().top() == transient);
    CHECK(runtime.views().get(second).input_states().base() == motion);
    runtime.views().reset_input_states(first);

    REQUIRE(runtime.buffers().get(buffer).modes().enable_minor(runtime.modes(), forced));
    CHECK(runtime.views().get(first).input_states().base() == transient);
    CHECK(runtime.views().get(second).input_states().base() == transient);
    REQUIRE(runtime.buffers().get(buffer).modes().disable_minor(forced));
    CHECK(runtime.views().get(first).input_states().base() == transient);
    runtime.set_default_input_strategy(alternate);
    CHECK(runtime.views().get(first).input_states().base() == transient);
    CHECK(runtime.views().get(second).input_states().base() == transient);
    runtime.set_default_input_strategy(modal);
    CHECK(runtime.views().get(first).input_states().base() == transient);
    CHECK(runtime.views().get(second).input_states().base() == motion);
    runtime.views().set_input_strategy(first, std::nullopt);
    CHECK_FALSE(runtime.views().get(first).input_strategy());
    CHECK(runtime.views().get(first).input_states().base() == motion);
    CHECK(runtime.modes().unsubscribe(listener));
}

TEST_CASE("buffers have stable identities and outlive their views") {
    EditorRuntime runtime;
    const BufferId first = runtime.buffers().create({.name = "main.cc",
                                                     .initial_text = "abc",
                                                     .kind = BufferKind::File,
                                                     .resource_uri = "file:///tmp/main.cc"});
    const BufferId second = runtime.buffers().create({.name = "main.cc",
                                                      .initial_text = "generated",
                                                      .kind = BufferKind::Generated,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});

    CHECK(runtime.buffers().get(first).name() == "main.cc");
    CHECK(runtime.buffers().get(second).name() == "main.cc<2>");
    CHECK(runtime.buffers().find_by_resource("file:///tmp/main.cc") == first);

    const ViewId view = runtime.views().create(first, TextOffset{1});
    CHECK_FALSE(runtime.buffers().erase(first));
    CHECK(runtime.views().erase(view));
    CHECK(runtime.buffers().erase(first));
    CHECK(runtime.buffers().try_get(first) == nullptr);

    const BufferId replacement = runtime.buffers().create({.name = "other",
                                                           .initial_text = "",
                                                           .kind = BufferKind::Scratch,
                                                           .resource_uri = std::nullopt,
                                                           .read_only = false});
    CHECK(replacement.slot == first.slot);
    CHECK(replacement.generation != first.generation);
    CHECK_THROWS_AS(runtime.buffers().get(first), std::out_of_range);
}

TEST_CASE("views keep independent positions backed by document anchors") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "shared",
                                                      .initial_text = "abcd",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId left = runtime.views().create(buffer, TextOffset{1});
    const ViewId right = runtime.views().create(buffer, TextOffset{3});

    auto transaction = runtime.buffers().get(buffer).begin_transaction();
    transaction.insert(TextOffset{1}, "X");
    transaction.commit();

    CHECK(runtime.views().caret(left).value == 2);
    CHECK(runtime.views().caret(right).value == 4);

    runtime.views().set_selection(left, {.anchor = TextOffset{1}, .head = TextOffset{4}});
    const std::optional<TextRange> selected = runtime.views().selection(left);
    REQUIRE(selected.has_value());
    CHECK(selected.value() == make_range(1, 4));
    CHECK_FALSE(runtime.views().selection(right).has_value());
}

TEST_CASE("windows bind views without merging buffer-relative display state") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "shared",
                                                      .initial_text = "first\nsecond\n",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId first = runtime.views().create(buffer, TextOffset{0});
    const ViewId second = runtime.views().create(buffer, TextOffset{6});
    runtime.views().get(first).viewport().top_line_offset = 0.25F;
    runtime.views().get(second).viewport().top_line = 1;

    const WindowId window = runtime.windows().create(first);
    CHECK(runtime.windows().get(window).view_id() == first);
    runtime.windows().set_view(window, second);
    CHECK(runtime.windows().get(window).view_id() == second);
    CHECK(runtime.views().get(first).attached_window_count() == 0);
    CHECK(runtime.views().get(second).attached_window_count() == 1);
    CHECK_FALSE(runtime.views().erase(second));
    CHECK(runtime.views().caret(first).value == 0);
    CHECK(runtime.views().caret(second).value == 6);
    CHECK(runtime.views().get(first).viewport().top_line_offset == doctest::Approx(0.25F));
    CHECK(runtime.views().get(second).viewport().top_line == 1);

    CHECK(runtime.windows().erase(window));
    CHECK(runtime.views().try_get(first) != nullptr);
    CHECK(runtime.views().try_get(second) != nullptr);
}

TEST_CASE("projects own tooling scope without owning editor windows") {
    EditorRuntime runtime;
    const SettingId server = runtime.setting_definitions().define(
        "language.server", std::string("default"),
        SettingScope::Application | SettingScope::Project | SettingScope::Buffer);
    const ProjectId outer = runtime.projects().create({.name = "outer", .roots = {"file:///work"}});
    const ProjectId inner =
        runtime.projects().create({.name = "inner", .roots = {"file:///work/sub"}});
    runtime.projects().get(inner).settings().set(server, std::string("clangd"));

    const BufferId buffer = runtime.buffers().create({.name = "main.cc",
                                                      .initial_text = "",
                                                      .kind = BufferKind::File,
                                                      .resource_uri = "file:///work/sub/main.cc",
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);

    CHECK(runtime.projects().find_for_resource("file:///work/readme") == outer);
    CHECK(runtime.projects().find_for_resource("file:///work/sub/main.cc") == inner);
    runtime.projects().assign(buffer, inner);
    CHECK(runtime.settings_for(buffer, view).get_as<std::string>(server) == "clangd");
    runtime.projects().begin_index(inner);
    CHECK(runtime.projects().get(inner).indexing());
    runtime.projects().replace_index(inner, {"/work/sub/z.cpp", "/work/sub/a.cpp"});
    CHECK(runtime.projects().get(inner).files() ==
          std::vector<std::string>{"/work/sub/a.cpp", "/work/sub/z.cpp"});
    CHECK(runtime.projects().get(inner).index_revision() == 1);
    CHECK_FALSE(runtime.projects().erase(inner));
    runtime.projects().assign(buffer, std::nullopt);
    CHECK(runtime.projects().erase(inner));
    CHECK(runtime.projects().try_get(inner) == nullptr);
}

TEST_CASE("commands receive explicit runtime buffer and view context") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "command",
                                                      .initial_text = "ab",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId first = runtime.views().create(buffer, TextOffset{0});
    const ViewId second = runtime.views().create(buffer, TextOffset{2});

    const CommandId insert = runtime.commands().define(
        "editor.insert",
        [](CommandContext& context, const CommandInvocation& invocation) -> CommandResult {
            if (invocation.arguments.size() != 1 ||
                !std::holds_alternative<std::string>(invocation.arguments.front())) {
                return std::unexpected(CommandError{"expected one string argument"});
            }
            const std::string& text = std::get<std::string>(invocation.arguments.front());
            auto transaction = context.buffer().begin_transaction();
            transaction.insert(context.runtime().views().caret(context.view_id()), text);
            transaction.commit();
            return CommandCompleted{};
        });

    const WindowId window = runtime.windows().create(first);
    CommandContext context(runtime, window, buffer, first);
    CHECK_THROWS_AS([&] { (void)CommandContext(runtime, window, buffer, second); }(),
                    std::invalid_argument);
    CommandResult result = runtime.commands().invoke(
        insert, context,
        CommandInvocation{.arguments = {std::string("X")}, .repeat_count = std::nullopt});
    REQUIRE(result.has_value());
    CHECK(runtime.buffers().get(buffer).snapshot().content() == "Xab");
    CHECK(runtime.views().caret(first).value == 1);
    CHECK(runtime.views().caret(second).value == 3);

    runtime.buffers().get(buffer).set_read_only(true);
    CHECK_THROWS_AS(
        [&] {
            (void)runtime.commands().invoke(
                insert, context,
                CommandInvocation{.arguments = {std::string("forbidden")},
                                  .repeat_count = std::nullopt});
        }(),
        std::logic_error);
}

TEST_CASE("keymaps resolve layered chords and command loop repeat counts") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "keys",
                                                      .initial_text = "",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);

    int base_calls = 0;
    std::optional<std::int64_t> received_repeat;
    const CommandId base = runtime.commands().define(
        "base.command", [&](CommandContext&, const CommandInvocation& invocation) -> CommandResult {
            ++base_calls;
            received_repeat = invocation.repeat_count;
            return CommandCompleted{};
        });
    int local_calls = 0;
    const CommandId local = runtime.commands().define(
        "local.command", [&](CommandContext&, const CommandInvocation&) -> CommandResult {
            ++local_calls;
            return CommandCompleted{};
        });

    const KeymapId local_map = runtime.keymaps().define("local");
    const KeymapId base_map = runtime.keymaps().define("base");
    runtime.keymaps().bind(base_map, "C-x C-f", base);
    runtime.keymaps().bind(base_map, "C-a", base);
    runtime.keymaps().bind(local_map, "C-a", local);

    CommandLoop loop(runtime);
    loop.set_keymap_layers(
        {{.keymap = local_map, .scope = "local"}, {.keymap = base_map, .scope = "global"}});
    const KeySequence chord = *parse_key_sequence("C-x C-f");
    CHECK(loop.dispatch(chord[0], context).status == CommandLoopStatus::Prefix);
    const std::vector<KeymapCompletion> completions =
        runtime.keymaps().completions(base_map, std::span(chord).first(1));
    REQUIRE(completions.size() == 1);
    CHECK(format_key_stroke(completions.front().key) == "C-f");
    CHECK(completions.front().command == base);
    loop.set_repeat_count(4);
    const CommandLoopResult chord_result = loop.dispatch(chord[1], context);
    CHECK(chord_result.status == CommandLoopStatus::Executed);
    CHECK(chord_result.key_sequence == "C-x C-f");
    CHECK(base_calls == 1);
    CHECK(received_repeat == 4);

    const KeyStroke control_a = parse_key_sequence("C-a")->front();
    CHECK(loop.dispatch(control_a, context).status == CommandLoopStatus::Executed);
    CHECK(local_calls == 1);
    CHECK(base_calls == 1);

    runtime.keymaps().bind(local_map, "C-x C-l", local);
    CHECK(loop.dispatch(chord[0], context).status == CommandLoopStatus::Prefix);
    CHECK(loop.pending_keymap() == local_map);
    const std::vector<KeymapCompletion> layered_completions = loop.pending_completions();
    CHECK(std::ranges::any_of(layered_completions, [](const KeymapCompletion& completion) {
        return format_key_stroke(completion.key) == "C-l";
    }));
    CHECK(std::ranges::any_of(layered_completions, [](const KeymapCompletion& completion) {
        return format_key_stroke(completion.key) == "C-f";
    }));
    const CommandLoopResult sparse_fallback = loop.dispatch(chord[1], context);
    CHECK(sparse_fallback.status == CommandLoopStatus::Executed);
    CHECK(local_calls == 1);
    CHECK(base_calls == 2);

    runtime.keymaps().bind(local_map, "C-x C-f", local);
    CHECK(loop.dispatch(chord[0], context).status == CommandLoopStatus::Prefix);
    CHECK(loop.dispatch(chord[1], context).status == CommandLoopStatus::Executed);
    CHECK(local_calls == 2);
    CHECK(base_calls == 2);

    runtime.keymaps().bind(base_map, "C-x", local);
    CHECK(runtime.keymaps().resolve(base_map, *parse_key_sequence("C-x")).command == local);
    CHECK(runtime.keymaps().resolve(base_map, chord).kind == KeymapMatchKind::None);

    runtime.keymaps().bind(base_map, "C-a C-b", local);
    CHECK(runtime.keymaps().resolve(base_map, *parse_key_sequence("C-a")).kind ==
          KeymapMatchKind::Prefix);
    CHECK(runtime.keymaps().resolve(base_map, *parse_key_sequence("C-a C-b")).command == local);
}

TEST_CASE("command loop contains command exceptions") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "errors",
                                                      .initial_text = "",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);

    const CommandId failing = runtime.commands().define(
        "test.fail", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            throw std::logic_error("buffer is read-only");
        });

    CommandLoop loop(runtime);
    const CommandLoopResult result = loop.execute(failing, context);
    CHECK(result.status == CommandLoopStatus::Error);
    CHECK(result.consumed);
    CHECK(result.command == failing);
    CHECK(result.message == "buffer is read-only");
}

TEST_CASE("keymap parents compose sparse local bindings") {
    EditorRuntime runtime;
    const CommandId inherited = runtime.commands().define(
        "parent.command", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    const CommandId local = runtime.commands().define(
        "child.command", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    const KeymapId parent = runtime.keymaps().define("parent");
    const KeymapId child = runtime.keymaps().define("child");
    runtime.keymaps().bind(parent, "C-a", inherited);
    runtime.keymaps().bind(parent, "C-x C-f", inherited);
    runtime.keymaps().bind(child, "C-b", local);
    runtime.keymaps().bind(child, "C-x C-l", local);
    runtime.keymaps().set_parent(child, parent);

    CHECK(runtime.keymaps().resolve(child, *parse_key_sequence("C-a")).command == inherited);
    CHECK(runtime.keymaps().resolve(child, *parse_key_sequence("C-b")).command == local);
    const KeySequence prefix = *parse_key_sequence("C-x");
    CHECK(runtime.keymaps().resolve(child, prefix).kind == KeymapMatchKind::Prefix);
    const std::vector<KeymapCompletion> completions = runtime.keymaps().completions(child, prefix);
    CHECK(std::ranges::any_of(completions, [](const KeymapCompletion& completion) {
        return format_key_stroke(completion.key) == "C-l";
    }));
    CHECK(std::ranges::any_of(completions, [](const KeymapCompletion& completion) {
        return format_key_stroke(completion.key) == "C-f";
    }));
    const std::vector<KeymapBinding> bindings = runtime.keymaps().bindings(child);
    CHECK(std::ranges::any_of(bindings, [inherited](const KeymapBinding& binding) {
        return format_key_sequence(binding.sequence) == "C-a" && binding.command == inherited;
    }));
    CHECK_THROWS_AS(runtime.keymaps().set_parent(parent, child), std::invalid_argument);
}

TEST_CASE("keymaps compose explicit prefix maps and one-pass command remaps") {
    EditorRuntime runtime;
    const auto command = [&](std::string name) {
        return runtime.commands().define(
            std::move(name), [](CommandContext&, const CommandInvocation&) -> CommandResult {
                return CommandCompleted{};
            });
    };
    const CommandId original = command("motion.original");
    const CommandId replacement = command("motion.replacement");
    const CommandId recursive_replacement = command("motion.recursive-replacement");
    const CommandId direct = command("motion.direct");

    const KeymapId base = runtime.keymaps().define("base");
    const KeymapId goto_map = runtime.keymaps().define("goto");
    const KeymapId minor = runtime.keymaps().define("minor");
    runtime.keymaps().bind(goto_map, "x", original);
    runtime.keymaps().bind_prefix(base, "g", goto_map, "Goto");
    runtime.keymaps().bind_remap(minor, original, replacement);
    runtime.keymaps().bind_remap(base, replacement, recursive_replacement);

    const std::vector<KeymapId> layers{minor, base};
    const KeymapMatch resolved = runtime.keymaps().resolve(layers, *parse_key_sequence("g x"));
    CHECK(resolved.kind == KeymapMatchKind::Command);
    CHECK(resolved.command == replacement);
    CHECK(resolved.source == base);

    const BufferId buffer = runtime.buffers().create({.name = "keymap-test",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    CommandLoop loop(runtime);
    loop.set_keymap_layers(
        {{.keymap = minor, .scope = "minor"}, {.keymap = base, .scope = "base"}});
    const KeySequence prefixed = *parse_key_sequence("g x");
    CHECK(loop.dispatch(prefixed.front(), context).status == CommandLoopStatus::Prefix);
    const CommandLoopResult dispatched = loop.dispatch(prefixed.back(), context);
    CHECK(dispatched.status == CommandLoopStatus::Executed);
    CHECK(dispatched.command == replacement);

    const std::vector<KeymapCompletion> root = runtime.keymaps().completions(base, {});
    const auto goto_prefix = std::ranges::find_if(root, [](const KeymapCompletion& completion) {
        return format_key_stroke(completion.key) == "g";
    });
    REQUIRE(goto_prefix != root.end());
    CHECK(goto_prefix->prefix);
    CHECK(goto_prefix->prefix_keymap == goto_map);
    CHECK(goto_prefix->label == "Goto");
    const std::vector<KeymapCompletion> nested =
        runtime.keymaps().completions(base, *parse_key_sequence("g"));
    REQUIRE(nested.size() == 1);
    CHECK(nested.front().command == original);
    const std::vector<KeymapCompletion> layered_nested =
        runtime.keymaps().completions(layers, *parse_key_sequence("g"));
    REQUIRE(layered_nested.size() == 1);
    CHECK(layered_nested.front().command == replacement);

    const std::vector<KeymapEntry> entries = runtime.keymaps().entries(base);
    CHECK(std::ranges::any_of(entries, [goto_map](const KeymapEntry& entry) {
        return format_key_sequence(entry.sequence) == "g" &&
               entry.kind == KeymapEntryKind::Prefix && entry.prefix_keymap == goto_map &&
               entry.label == "Goto";
    }));
    CHECK(std::ranges::any_of(entries, [original](const KeymapEntry& entry) {
        return format_key_sequence(entry.sequence) == "g x" && entry.command == original;
    }));
    CHECK(runtime.keymaps().remaps(minor) ==
          std::vector<KeymapRemap>{{.command = original, .replacement = replacement}});

    runtime.keymaps().bind(base, "g", direct);
    CHECK(runtime.keymaps().resolve(base, *parse_key_sequence("g")).command == direct);
    CHECK(runtime.keymaps().resolve(base, *parse_key_sequence("g x")).kind ==
          KeymapMatchKind::None);
    runtime.keymaps().bind_prefix(base, "g", goto_map, "Goto");
    CHECK_THROWS_AS(runtime.keymaps().bind_prefix(goto_map, "z", base), std::invalid_argument);
}

TEST_CASE("input states are registered globally and stacked per view") {
    EditorRuntime runtime;
    const KeymapId normal_map = runtime.keymaps().define("state.normal");
    const KeymapId transient_map = runtime.keymaps().define("state.transient");
    const InputStateId emacs = runtime.input_states().define({.name = "emacs",
                                                              .keymaps = {},
                                                              .text_input = TextInputPolicy::Accept,
                                                              .cursor = CursorShape::Beam,
                                                              .indicator = "E",
                                                              .handler = {}});
    const InputStateId normal =
        runtime.input_states().define({.name = "normal",
                                       .keymaps = {normal_map},
                                       .text_input = TextInputPolicy::Ignore,
                                       .cursor = CursorShape::Block,
                                       .indicator = "N",
                                       .handler = {}});
    const InputStateId transient =
        runtime.input_states().define({.name = "transient",
                                       .keymaps = {transient_map},
                                       .text_input = TextInputPolicy::Ignore,
                                       .cursor = CursorShape::Underline,
                                       .indicator = "K",
                                       .handler = {}});

    const BufferId buffer = runtime.buffers().create({.name = "state-test",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId left = runtime.views().create(buffer);
    const ViewId right = runtime.views().create(buffer);
    CHECK_THROWS_AS(runtime.views().push_input_state(left, transient), std::logic_error);
    std::vector<InputStateChange> changes;
    const InputStateRegistry::ListenerId listener = runtime.input_states().subscribe(
        [&](const InputStateChange& change) { changes.push_back(change); });

    runtime.views().set_base_input_state(left, emacs);
    runtime.views().set_base_input_state(right, normal);
    runtime.views().push_input_state(left, transient);
    runtime.views().set_input_feedback(
        left,
        {.sequence = "C-x", .hints = {{.key = "C-s", .detail = "file.save", .prefix = false}}});
    REQUIRE(runtime.views().get(left).input_states().feedback());
    runtime.views().set_base_input_state(left, normal);
    const ViewInputStates& left_states = runtime.views().get(left).input_states();
    const ViewInputStates& right_states = runtime.views().get(right).input_states();

    REQUIRE(left_states.stack().size() == 2);
    CHECK(left_states.stack()[0] == normal);
    CHECK(left_states.stack()[1] == transient);
    REQUIRE(right_states.stack().size() == 1);
    CHECK(right_states.stack()[0] == normal);
    CHECK(left_states.top() == transient);
    CHECK(left_states.base() == normal);
    CHECK_FALSE(left_states.feedback().has_value());
    CHECK(runtime.input_states().definition(transient).indicator == "K");
    CHECK(changes.size() == 4);
    CHECK(changes[2] ==
          InputStateChange{
              .view = left, .kind = InputStateChangeKind::Push, .from = emacs, .to = transient});
    CHECK(changes[3] ==
          InputStateChange{
              .view = left, .kind = InputStateChangeKind::Base, .from = emacs, .to = normal});

    CHECK(runtime.views().pop_input_state(left) == transient);
    CHECK_FALSE(runtime.views().pop_input_state(left).has_value());
    runtime.views().push_input_state(left, transient);
    runtime.views().push_input_state(left, emacs);
    runtime.views().reset_input_states(left);
    REQUIRE(left_states.stack().size() == 1);
    CHECK(left_states.stack()[0] == normal);

    const ViewId disposable = runtime.views().create(buffer);
    runtime.views().set_base_input_state(disposable, normal);
    runtime.views().push_input_state(disposable, transient);
    changes.clear();
    REQUIRE(runtime.views().erase(disposable));
    REQUIRE(changes.size() == 1);
    CHECK(changes.front() == InputStateChange{.view = disposable,
                                              .kind = InputStateChangeKind::Pop,
                                              .from = transient,
                                              .to = normal});

    CHECK(runtime.input_states().unsubscribe(listener));
    CHECK_FALSE(runtime.input_states().unsubscribe(listener));
    CHECK_THROWS_AS(runtime.views().push_input_state(left, InputStateId{}), std::out_of_range);
}

TEST_CASE("focused text input moves and deletes by grapheme cluster") {
    TextInput input("a👩‍💻b");
    CHECK(input.caret() == input.text().size());
    REQUIRE(input.move_backward());
    CHECK(input.caret() == input.text().size() - 1);
    REQUIRE(input.move_backward());
    CHECK(input.caret() == 1);
    REQUIRE(input.move_forward());
    REQUIRE(input.erase_backward());
    CHECK(input.text() == "ab");
    CHECK(input.caret() == 1);
}

TEST_CASE("interaction controller owns non-blocking command input") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "prompt",
                                                      .initial_text = "",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);

    std::string submitted;
    const CommandId accept = runtime.commands().define(
        "prompt.accept",
        [&](CommandContext&, const CommandInvocation& invocation) -> CommandResult {
            REQUIRE(invocation.arguments.size() == 1);
            submitted = std::get<std::string>(invocation.arguments.front());
            return CommandCompleted{};
        });
    const CommandId start = runtime.commands().define(
        "prompt.start", [accept](CommandContext&, const CommandInvocation&) -> CommandResult {
            return InteractionRequest{.kind = InteractionKind::Text,
                                      .prompt = "find: ",
                                      .initial_input = "needle",
                                      .history = "search",
                                      .provider = {},
                                      .allow_custom_input = true,
                                      .accept_command = accept,
                                      .arguments = {}};
        });
    const KeymapId keymap = runtime.keymaps().define("prompt-test");
    runtime.keymaps().bind(keymap, "C-f", start);

    CommandLoop loop(runtime);
    loop.set_keymap_layers({{.keymap = keymap, .scope = "test"}});
    const CommandLoopResult started = loop.dispatch(parse_key_sequence("C-f")->front(), context);
    CHECK(started.status == CommandLoopStatus::AwaitingInput);
    REQUIRE(started.interaction.has_value());
    InteractionController interaction(runtime.interaction_providers());
    REQUIRE(interaction.start(*started.interaction, context).has_value());
    REQUIRE(interaction.state() != nullptr);
    CHECK(interaction.state()->input.text() == "needle");
    CHECK(interaction.state()->input.caret() == 6);

    interaction.insert("é", context);
    CHECK(interaction.erase_backward(context));
    CHECK(interaction.state()->input.text() == "needle");
    CHECK(interaction.state()->input.caret() == 6);
    interaction.insert("-next", context);
    const std::expected<InteractionSubmission, std::string> submission = interaction.submit();
    REQUIRE(submission.has_value());
    CHECK(loop.execute(submission->accept_command, context, submission->invocation).status ==
          CommandLoopStatus::Executed);
    CHECK(submitted == "needle-next");
    CHECK_FALSE(interaction.active());
}

TEST_CASE("async interaction providers discard cancelled generations") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "prompt",
                                                      .initial_text = "",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    const CommandId accept = runtime.commands().define(
        "async.accept", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });

    std::latch first_started(1);
    std::latch release_first(1);
    runtime.interaction_providers().define(
        "async", [&](CommandContext&, std::string_view query) -> InteractionProviderResult {
            return InteractionCandidateWork{[query = std::string(query), &first_started,
                                             &release_first](const std::stop_token& cancellation) {
                if (query == "a") {
                    first_started.count_down();
                    release_first.wait();
                }
                if (cancellation.stop_requested()) {
                    throw AsyncTaskCancelled();
                }
                return std::vector<InteractionCandidate>{
                    {.value = query, .label = query, .detail = "async", .filter_text = query}};
            }};
        });

    WakeSignal wake;
    AsyncRuntime async([&wake] { wake.notify(); });
    InteractionController interaction(runtime.interaction_providers());
    interaction.attach_async_runtime(async);
    REQUIRE(interaction
                .start({.kind = InteractionKind::Picker,
                        .prompt = "async: ",
                        .initial_input = "a",
                        .history = {},
                        .provider = "async",
                        .allow_custom_input = false,
                        .accept_command = accept,
                        .arguments = {}},
                       context)
                .has_value());
    REQUIRE(interaction.state() != nullptr);
    CHECK(interaction.state()->loading);
    first_started.wait();

    interaction.insert("b", context);
    REQUIRE(wake.wait());
    CHECK(async.drain() == 1);
    REQUIRE(interaction.state() != nullptr);
    REQUIRE(interaction.state()->candidates.size() == 1);
    CHECK(interaction.state()->candidates.front().value == "ab");
    CHECK_FALSE(interaction.state()->loading);

    release_first.count_down();
    REQUIRE(wake.wait());
    CHECK(async.drain() == 1);
    REQUIRE(interaction.state() != nullptr);
    REQUIRE(interaction.state()->candidates.size() == 1);
    CHECK(interaction.state()->candidates.front().value == "ab");
}
