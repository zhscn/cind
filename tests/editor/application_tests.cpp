#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/command_loop.hpp"
#include "editor/completion.hpp"
#include "editor/interaction.hpp"
#include "editor/language_mechanism.hpp"
#include "editor/runtime.hpp"
#include "editor/workbench.hpp"
#include "editor/workbench_session.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <memory>
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

void insert_interaction_text(EditorRuntime& runtime, const InteractionMechanisms& interaction,
                             std::string_view text) {
    const InteractionMechanismState* state = interaction.state();
    REQUIRE(state != nullptr);
    EditTransaction transaction = runtime.buffers().get(state->buffer).begin_transaction();
    transaction.insert(runtime.views().caret(state->view), text);
    (void)transaction.commit();
}

InputStateId define_test_input_state(EditorRuntime& runtime, std::string name) {
    return runtime.input_states().define({.name = std::move(name),
                                          .keymaps = {},
                                          .text_input = TextInputPolicy::Accept,
                                          .text_command = std::string("edit.self-insert"),
                                          .cursor = CursorShape::Beam,
                                          .indicator = {},
                                          .handler = {},
                                          .position_hints = {},
                                          .on_enter = {},
                                          .on_exit = {}});
}

} // namespace

TEST_CASE("workbench registry preserves independent layouts") {
    WorkbenchRegistry registry;
    const WindowId first_window{0, 1};
    const WindowId second_window{1, 1};
    const WindowId third_window{2, 1};

    const WorkbenchId first = registry.create({.root_window = first_window});
    CHECK(registry.active_id() == first);
    REQUIRE(registry.get(first).layout().split(
        {.target = first_window, .new_window = third_window, .axis = WindowSplitAxis::Columns}));

    const WorkbenchId second = registry.create({.root_window = second_window});
    CHECK_THROWS_AS(registry.create({.root_window = second_window}), std::invalid_argument);
    CHECK(registry.active_id() == first);
    REQUIRE(registry.find_by_window(second_window).has_value());
    CHECK(*registry.find_by_window(second_window) == second);
    REQUIRE(registry.next(first).has_value());
    CHECK(*registry.next(first) == second);
    CHECK(registry.activate(second));
    CHECK(registry.active().layout().contains(second_window));

    CHECK(registry.erase(first));
    CHECK(registry.try_get(first) == nullptr);
    CHECK_THROWS_AS(registry.erase(second), std::logic_error);
}

TEST_CASE("workbench session codec preserves stable layout state and rejects corruption") {
    WorkbenchLayoutSessionState layout{
        .window = std::nullopt,
        .axis = WindowSplitAxis::Rows,
        .ratio = 0.72F,
        .first = std::make_unique<WorkbenchLayoutSessionState>(WorkbenchLayoutSessionState{
            .window = WorkbenchWindowSessionState{.resource = "/tmp/source.cc",
                                                  .caret = 17,
                                                  .role = std::nullopt,
                                                  .pinned = true,
                                                  .created_by_policy = false,
                                                  .jump_walk = {},
                                                  .jump_cursor = std::nullopt},
            .axis = WindowSplitAxis::Rows,
            .ratio = 0.5F,
            .first = nullptr,
            .second = nullptr}),
        .second = std::make_unique<WorkbenchLayoutSessionState>(WorkbenchLayoutSessionState{
            .window = WorkbenchWindowSessionState{.resource = "/tmp/results",
                                                  .caret = 3,
                                                  .role = "tools",
                                                  .pinned = false,
                                                  .created_by_policy = true,
                                                  .jump_walk = {7, 8},
                                                  .jump_cursor = 1},
            .axis = WindowSplitAxis::Rows,
            .ratio = 0.5F,
            .first = nullptr,
            .second = nullptr})};
    WorkbenchSessionState state{.version = WorkbenchSessionState::current_version,
                                .active_workbench = 1,
                                .workbenches = {}};
    state.workbenches.push_back(
        {.name = {},
         .scope_roots = {"/tmp/project"},
         .mru_resources = {"/tmp/source.cc"},
         .layout = WorkbenchLayoutSessionState{.window = WorkbenchWindowSessionState{},
                                               .axis = WindowSplitAxis::Rows,
                                               .ratio = 0.5F,
                                               .first = nullptr,
                                               .second = nullptr},
         .active_leaf = 0,
         .jump_nodes = {},
         .jump_edges = {}});
    state.workbenches.push_back(
        {.name = "shop \"frontend\"",
         .scope_roots = {"/tmp/backend", "/tmp/frontend"},
         .mru_resources = {"/tmp/results", "/tmp/source.cc"},
         .layout = std::move(layout),
         .active_leaf = 1,
         .jump_nodes = {{.id = 7,
                         .resource = "/tmp/source.cc",
                         .fallback = {.line = 3, .byte_column = 4},
                         .excerpt = "source line",
                         .created_at = 11,
                         .last_visit = 17},
                        {.id = 8,
                         .resource = "/tmp/results",
                         .fallback = {.line = 9, .byte_column = 2},
                         .excerpt = "result line",
                         .created_at = 12,
                         .last_visit = 18}},
         .jump_edges = {{.from = 7, .to = 8, .kind = "manual", .at = 19, .persistent = true}}});

    const std::string serialized = serialize_workbench_session(state);
    const std::expected<WorkbenchSessionState, std::string> parsed =
        parse_workbench_session(serialized);
    REQUIRE(parsed.has_value());
    CHECK(parsed->active_workbench == 1);
    REQUIRE(parsed->workbenches.size() == 2);
    const WorkbenchSessionEntry& restored = parsed->workbenches[1];
    CHECK(restored.name == "shop \"frontend\"");
    CHECK(restored.scope_roots == std::vector<std::string>{"/tmp/backend", "/tmp/frontend"});
    CHECK(restored.mru_resources == std::vector<std::string>{"/tmp/results", "/tmp/source.cc"});
    CHECK(restored.active_leaf == 1);
    CHECK_FALSE(restored.layout.leaf());
    CHECK(restored.layout.axis == WindowSplitAxis::Rows);
    CHECK(restored.layout.ratio == doctest::Approx(0.72F));
    REQUIRE(restored.layout.first->window.has_value());
    CHECK(restored.layout.first->window->resource == std::optional<std::string>{"/tmp/source.cc"});
    CHECK(restored.layout.first->window->caret == 17);
    CHECK(restored.layout.first->window->pinned);
    REQUIRE(restored.layout.second->window.has_value());
    CHECK(restored.layout.second->window->role == std::optional<std::string>{"tools"});
    CHECK(restored.layout.second->window->created_by_policy);
    CHECK(restored.layout.second->window->jump_walk == std::vector<std::uint64_t>{7, 8});
    CHECK(restored.layout.second->window->jump_cursor == std::optional<std::size_t>{1});
    REQUIRE(restored.jump_nodes.size() == 2);
    CHECK(restored.jump_nodes[0].resource == "/tmp/source.cc");
    CHECK(restored.jump_nodes[1].fallback == LinePosition{.line = 9, .byte_column = 2});
    REQUIRE(restored.jump_edges.size() == 1);
    CHECK(restored.jump_edges[0].kind == "manual");
    CHECK(restored.jump_edges[0].persistent);

    CHECK_FALSE(parse_workbench_session("cind-workbench-session 2 0 1").has_value());
    CHECK_FALSE(parse_workbench_session(
                    "cind-workbench-session 1 0 1\nworkbench \"x\" 0 0 0\nbranch rows 2\n")
                    .has_value());
    CHECK_FALSE(parse_workbench_session(serialized + "trailing").has_value());
}

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

TEST_CASE("resource policies are named, ordered, transactional registry data") {
    EditorRuntime runtime;
    const ModeId text = runtime.modes().define("text", ModeKind::Major);
    const ModeId scheme = runtime.modes().define("scheme", ModeKind::Major);
    const ModeId minor = runtime.modes().define("minor", ModeKind::Minor);

    runtime.resource_policies().define_file_mode("source", text, {".scm"});
    runtime.resource_policies().define_file_mode("user-source", scheme, {".scm"});
    CHECK(runtime.resource_policies().mode_for("init.scm") == scheme);
    runtime.resource_policies().define_file_mode("user-source", text, {}, {"guix.scm"});
    CHECK(runtime.resource_policies().mode_for("guix.scm") == text);
    CHECK(runtime.resource_policies().mode_for("init.scm") == text);
    CHECK_THROWS_AS(runtime.resource_policies().define_file_mode("bad", minor, {".bad"}),
                    std::invalid_argument);

    runtime.resource_policies().define_project_provider("vcs", {".git"});
    const EditorRuntime::ExtensionCheckpoint checkpoint = runtime.checkpoint_extensions();
    runtime.resource_policies().define_project_provider("user", {"project.scm"});
    REQUIRE(runtime.resource_policies().project_providers().size() == 2);
    runtime.restore_extensions(checkpoint);
    REQUIRE(runtime.resource_policies().project_providers().size() == 1);
    CHECK(runtime.resource_policies().project_providers().front().name == "vcs");

    runtime.seal_extensions();
    CHECK_THROWS_AS(runtime.resource_policies().define_project_provider("late", {"late"}),
                    std::logic_error);
}

TEST_CASE("buffers expose validated semantic source locations") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "locations",
                                                      .initial_text = "first\nsecond\n",
                                                      .kind = BufferKind::Generated,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = true});
    runtime.buffers().set_locations(
        buffer, {{.source_range = make_range(0, 6),
                  .resource = "/work/first.cpp",
                  .target = {.line = 4, .column = 2, .encoding = PositionEncoding::Bytes},
                  .excerpt = {}},
                 {.source_range = make_range(6, 13),
                  .resource = "/work/second.cpp",
                  .target = {.line = 8, .column = 1, .encoding = PositionEncoding::Bytes},
                  .excerpt = {}}});

    const Buffer& result = runtime.buffers().get(buffer);
    REQUIRE(result.location_at(TextOffset{2}) != nullptr);
    CHECK(result.location_at(TextOffset{2})->resource == "/work/first.cpp");
    REQUIRE(result.location_at(result.snapshot().content().end_offset()) != nullptr);
    CHECK_THROWS_AS(runtime.buffers().set_locations(buffer, {{.source_range = make_range(0, 7),
                                                              .resource = "/work/a.cpp",
                                                              .target = {},
                                                              .excerpt = {}},
                                                             {.source_range = make_range(6, 8),
                                                              .resource = "/work/b.cpp",
                                                              .target = {},
                                                              .excerpt = {}}}),
                    std::invalid_argument);
    Buffer& mutable_result = runtime.buffers().get(buffer);
    mutable_result.set_read_only(false);
    auto transaction = mutable_result.begin_transaction();
    transaction.insert(mutable_result.snapshot().content().end_offset(), "changed");
    transaction.commit();
    CHECK(mutable_result.locations().empty());
}

TEST_CASE("buffers merge revision-bound diagnostics by producer") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "diagnostics.cpp",
                                                      .initial_text = "one\ntwo\n",
                                                      .kind = BufferKind::File,
                                                      .resource_uri = "/work/diagnostics.cpp",
                                                      .read_only = false});
    const RevisionId revision = runtime.buffers().get(buffer).snapshot().revision();
    runtime.buffers().set_diagnostics(buffer, "compiler", revision,
                                      {{.range = make_range(4, 7),
                                        .severity = DiagnosticSeverity::Warning,
                                        .message = "warning",
                                        .source = "compiler",
                                        .code = "W1"}});
    runtime.buffers().set_diagnostics(buffer, "lsp", revision,
                                      {{.range = make_range(0, 3),
                                        .severity = DiagnosticSeverity::Error,
                                        .message = "error",
                                        .source = "lsp",
                                        .code = "E1"}});

    Buffer& source = runtime.buffers().get(buffer);
    REQUIRE(source.diagnostics().size() == 2);
    CHECK(source.diagnostics()[0].message == "error");
    CHECK(source.diagnostics()[1].message == "warning");
    CHECK(runtime.buffers().clear_diagnostics(buffer, "compiler"));
    REQUIRE(source.diagnostics().size() == 1);
    CHECK_FALSE(runtime.buffers().clear_diagnostics(buffer, "compiler"));
    CHECK_THROWS_AS(runtime.buffers().set_diagnostics(buffer, "", revision,
                                                      {{.range = make_range(0, 1),
                                                        .severity = DiagnosticSeverity::Error,
                                                        .message = "invalid owner",
                                                        .source = {},
                                                        .code = {}}}),
                    std::invalid_argument);

    auto transaction = source.begin_transaction();
    transaction.insert(TextOffset{0}, "x");
    (void)transaction.commit();
    CHECK(source.diagnostics().empty());
}

TEST_CASE("runtime noun registries own validated named mechanisms") {
    EditorRuntime runtime;
    const ThingId angle = runtime.things().define(
        "angle", {.kind = ThingPatternKind::Pair, .arguments = {"<", ">"}, .alternatives = {}});
    const ThingId symbol = runtime.things().define(
        "symbol", {.kind = ThingPatternKind::Multi,
                   .arguments = {},
                   .alternatives = {{.kind = ThingPatternKind::CstNode,
                                     .arguments = {"string-literal"},
                                     .alternatives = {}},
                                    {.kind = ThingPatternKind::CharacterClass,
                                     .arguments = {"symbol"},
                                     .alternatives = {}}}});
    CHECK(runtime.things().definition(angle).pattern.arguments ==
          std::vector<std::string>{"<", ">"});
    CHECK(runtime.things().find("symbol") == symbol);
    runtime.things().configure(
        angle, {.kind = ThingPatternKind::Pair, .arguments = {"(", ")"}, .alternatives = {}});
    CHECK(runtime.things().definition(angle).pattern.arguments ==
          std::vector<std::string>{"(", ")"});
    CHECK_THROWS_AS(
        runtime.things().define(
            "bad", {.kind = ThingPatternKind::Multi, .arguments = {}, .alternatives = {}}),
        std::invalid_argument);

    const MotionId word = runtime.motions().define("word", MotionMechanism::ForwardWord);
    CHECK(runtime.motions().definition(word).mechanism == MotionMechanism::ForwardWord);
    runtime.motions().configure(word, MotionMechanism::BackwardWord);
    CHECK(runtime.motions().definition(word).mechanism == MotionMechanism::BackwardWord);

    runtime.seal_extensions();
    CHECK_THROWS_AS(
        runtime.things().configure(
            angle, {.kind = ThingPatternKind::Pair, .arguments = {"[", "]"}, .alternatives = {}}),
        std::logic_error);
    CHECK_THROWS_AS(runtime.motions().define("late", MotionMechanism::UpList), std::logic_error);
}

TEST_CASE("major modes select composed language profiles instead of inheriting parsers") {
    EditorRuntime runtime;
    const auto mechanism = std::make_shared<LanguageMechanism>(
        LanguageFacet::Lexing | LanguageFacet::Syntax | LanguageFacet::Indentation |
            LanguageFacet::StructuralEditing,
        [] { return std::make_unique<LanguageMechanismSession>(); });
    const SettingId dialect = runtime.setting_definitions().define(
        "test.language.dialect", std::string("c++"),
        SettingScope::Language | SettingScope::Project | SettingScope::Buffer);

    const LanguageProviderId lexer = runtime.languages().define_provider(
        "test.language.lexer", LanguageFacet::Lexing, mechanism);
    const LanguageProviderId syntax = runtime.languages().define_provider(
        "test.language.syntax", LanguageFacet::Syntax, mechanism);
    const LanguageProviderId indentation = runtime.languages().define_provider(
        "test.language.indentation", LanguageFacet::Indentation, mechanism);
    const LanguageProviderId editing = runtime.languages().define_provider(
        "test.language.structural-editing", LanguageFacet::StructuralEditing, mechanism);
    CHECK_THROWS_AS(runtime.languages().define_provider("test.language.invalid",
                                                        LanguageFacet::Formatting, mechanism),
                    std::invalid_argument);

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

TEST_CASE("mode policy inheritance rederives every view input state") {
    EditorRuntime runtime;
    const auto input_state = [&](std::string name) {
        return runtime.input_states().define({.name = std::move(name),
                                              .keymaps = {},
                                              .text_input = TextInputPolicy::Ignore,
                                              .text_command = std::nullopt,
                                              .cursor = CursorShape::Block,
                                              .indicator = {},
                                              .handler = {},
                                              .position_hints = {},
                                              .on_enter = {},
                                              .on_exit = {}});
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
    runtime.modes().set_things(fundamental, {{.name = "word", .definition = "character"}});
    runtime.modes().set_completion_providers(fundamental, std::vector<std::string>{"word", "path"});
    runtime.modes().set_completion_auto(fundamental, true);
    runtime.modes().add_keymap(fundamental, parent_map);

    const KeymapId child_map = runtime.keymaps().define("test.child.map");
    const ModeId special = runtime.modes().define("special", ModeKind::Major);
    runtime.modes().set_parent(special, fundamental);
    runtime.modes().set_interaction_class(special, InteractionClass::Interface);
    runtime.modes().set_things(special, {{.name = "word", .definition = "interface"},
                                         {.name = "item", .definition = "line"}});
    runtime.modes().add_keymap(special, child_map);
    CHECK(runtime.keymaps().parent(child_map) == parent_map);
    CHECK(runtime.modes().effective_keymaps(special) == std::vector<KeymapId>{child_map});
    CHECK(runtime.keymaps().resolve(child_map, *parse_key_sequence("p")).command == parent_command);

    const ModeId editable = runtime.modes().define("editable", ModeKind::Minor);
    runtime.modes().set_interaction_class(editable, InteractionClass::Editing);
    runtime.modes().set_completion_providers(editable,
                                             std::vector<std::string>{"semantic", "word"});
    const ModeId forced = runtime.modes().define("forced", ModeKind::Minor);
    runtime.modes().set_initial_state(forced, transient);
    const ModeId completion_muted = runtime.modes().define("completion-muted", ModeKind::Minor);
    runtime.modes().set_completion_providers(completion_muted, std::vector<std::string>{});
    runtime.modes().set_completion_auto(completion_muted, false);
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
    CHECK(initial.things ==
          std::vector<ModeThingBinding>{{.name = "word", .definition = "interface"},
                                        {.name = "item", .definition = "line"}});
    CHECK(initial.completion_providers == std::vector<std::string>{"word", "path"});
    CHECK(initial.completion_auto);

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
    CHECK(changes.back().after.completion_providers ==
          std::vector<std::string>{"semantic", "word"});
    CHECK(runtime.views().get(first).input_states().base() == normal);
    CHECK(runtime.views().get(second).input_states().base() == normal);

    runtime.views().push_input_state(first, transient);
    REQUIRE(runtime.buffers().get(buffer).modes().disable_minor(editable));
    CHECK(runtime.modes()
              .effective_policy(runtime.buffers().get(buffer).modes())
              .completion_providers == std::vector<std::string>{"word", "path"});
    CHECK(runtime.views().get(first).input_states().base() == transient);
    CHECK(runtime.views().get(first).input_states().top() == transient);
    CHECK(runtime.views().get(second).input_states().base() == motion);
    runtime.views().reset_input_states(first);

    REQUIRE(runtime.buffers().get(buffer).modes().enable_minor(runtime.modes(), completion_muted));
    CHECK(runtime.modes()
              .effective_policy(runtime.buffers().get(buffer).modes())
              .completion_providers.empty());
    CHECK_FALSE(
        runtime.modes().effective_policy(runtime.buffers().get(buffer).modes()).completion_auto);
    REQUIRE(runtime.buffers().get(buffer).modes().disable_minor(completion_muted));

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

TEST_CASE("semantic mode policy changes preserve the view input state stack") {
    EditorRuntime runtime;
    const auto input_state = [&](std::string name) {
        return runtime.input_states().define({.name = std::move(name),
                                              .keymaps = {},
                                              .text_input = TextInputPolicy::Ignore,
                                              .text_command = std::nullopt,
                                              .cursor = CursorShape::Block,
                                              .indicator = {},
                                              .handler = {},
                                              .position_hints = {},
                                              .on_enter = {},
                                              .on_exit = {}});
    };
    const InputStateId normal = input_state("normal");
    const InputStateId insert = input_state("insert");
    const InputStateId capture = input_state("capture");
    runtime.set_default_input_strategy(runtime.input_strategies().define(
        {.name = "modal", .editing = normal, .interface = normal}));

    const ModeId programming = runtime.modes().define("programming", ModeKind::Major);
    runtime.modes().set_interaction_class(programming, InteractionClass::Editing);
    const ModeId semantic = runtime.modes().define("semantic", ModeKind::Minor);
    runtime.modes().set_things(semantic, {{.name = "item", .definition = "cind.word"}});

    const BufferId buffer = runtime.buffers().create({.name = "source",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    BufferModes& modes = runtime.buffers().get(buffer).modes();
    modes.set_major(runtime.modes(), programming);
    const ViewId view = runtime.views().create(buffer);
    runtime.views().set_base_input_state(view, insert);
    runtime.views().push_input_state(view, capture);

    REQUIRE(modes.enable_minor(runtime.modes(), semantic));
    CHECK(runtime.views().get(view).input_states().stack() ==
          std::vector<InputStateId>{insert, capture});
    CHECK(runtime.modes().effective_policy(modes).things ==
          std::vector<ModeThingBinding>{{.name = "item", .definition = "cind.word"}});

    REQUIRE(modes.disable_minor(semantic));
    CHECK(runtime.views().get(view).input_states().stack() ==
          std::vector<InputStateId>{insert, capture});
    CHECK(runtime.modes().effective_policy(modes).things.empty());
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

TEST_CASE("views retain typed multi-range selections through document edits") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "shared",
                                                      .initial_text = "abcdef",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{0});

    runtime.views().set_selection(
        view, ViewSelection{.ranges = {{.anchor = TextOffset{4},
                                        .head = TextOffset{1},
                                        .granularity = SelectionGranularity::Character},
                                       {.anchor = TextOffset{2},
                                        .head = TextOffset{6},
                                        .granularity = SelectionGranularity::Node}},
                            .primary = 1,
                            .metadata = "(thing . defun)"});

    CHECK(runtime.views().caret(view) == TextOffset{6});
    CHECK(runtime.views().mark(view) == TextOffset{2});
    REQUIRE(runtime.views().active_selection(view).has_value());
    CHECK(runtime.views().active_selection(view)->ranges[0].anchor == TextOffset{4});
    CHECK(runtime.views().active_selection(view)->ranges[0].head == TextOffset{1});
    CHECK(runtime.views().active_selection(view)->ranges[1].granularity ==
          SelectionGranularity::Node);
    CHECK(runtime.views().active_selection(view)->primary == 1);
    CHECK(runtime.views().active_selection(view)->metadata == "(thing . defun)");

    auto transaction = runtime.buffers().get(buffer).begin_transaction();
    transaction.insert(TextOffset{0}, "X");
    transaction.commit();

    const ViewSelection settled = runtime.views().selection_model(view);
    CHECK(settled.ranges[0].anchor == TextOffset{5});
    CHECK(settled.ranges[0].head == TextOffset{2});
    CHECK(settled.ranges[1].anchor == TextOffset{3});
    CHECK(settled.ranges[1].head == TextOffset{7});

    runtime.views().set_caret(view, TextOffset{4});
    const ViewSelection moved = runtime.views().selection_model(view);
    CHECK(moved.ranges[0] == settled.ranges[0]);
    CHECK(moved.ranges[1].anchor == TextOffset{3});
    CHECK(moved.ranges[1].head == TextOffset{4});

    runtime.views().clear_selection(view);
    CHECK_FALSE(runtime.views().active_selection(view).has_value());
    CHECK(runtime.views().caret(view) == TextOffset{4});
    const ViewSelection collapsed = runtime.views().selection_model(view);
    REQUIRE(collapsed.ranges.size() == 1);
    CHECK(collapsed.ranges[0].anchor == TextOffset{4});
    CHECK(collapsed.ranges[0].head == TextOffset{4});
    CHECK(collapsed.primary == 0);
    CHECK(collapsed.metadata == "()");
}

TEST_CASE("view selection history settles anchored multi-range snapshots") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "history",
                                                      .initial_text = "abcdef",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const ViewSelection stored{.ranges = {{.anchor = TextOffset{4},
                                           .head = TextOffset{1},
                                           .granularity = SelectionGranularity::Node},
                                          {.anchor = TextOffset{2},
                                           .head = TextOffset{5},
                                           .granularity = SelectionGranularity::Character}},
                               .primary = 1,
                               .metadata = "((history . structural))"};

    runtime.views().push_selection_history(view, stored);
    CHECK(runtime.views().selection_history_size(view) == 1);
    auto transaction = runtime.buffers().get(buffer).begin_transaction();
    transaction.insert(TextOffset{0}, "X");
    (void)transaction.commit();

    const std::optional<ViewSelection> settled = runtime.views().pop_selection_history(view);
    REQUIRE(settled.has_value());
    CHECK(settled->ranges[0].anchor == TextOffset{5});
    CHECK(settled->ranges[0].head == TextOffset{2});
    CHECK(settled->ranges[0].granularity == SelectionGranularity::Node);
    CHECK(settled->ranges[1].anchor == TextOffset{3});
    CHECK(settled->ranges[1].head == TextOffset{6});
    CHECK(settled->primary == 1);
    CHECK(settled->metadata == "((history . structural))");
    CHECK(runtime.views().selection_history_size(view) == 0);
    CHECK_FALSE(runtime.views().pop_selection_history(view).has_value());

    runtime.views().push_selection_history(view, stored);
    runtime.views().push_selection_history(view, stored);
    runtime.views().clear_selection_history(view);
    CHECK(runtime.views().selection_history_size(view) == 0);
}

TEST_CASE("view selections validate their non-empty primary range contract") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "shared",
                                                      .initial_text = "abc",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);

    CHECK_THROWS_AS(runtime.views().set_selection(view, ViewSelection{}), std::invalid_argument);
    CHECK_THROWS_AS(
        runtime.views().set_selection(
            view, ViewSelection{.ranges = {{.anchor = TextOffset{0},
                                            .head = TextOffset{1},
                                            .granularity = SelectionGranularity::Character}},
                                .primary = 1,
                                .metadata = "()"}),
        std::out_of_range);
    CHECK(runtime.views().caret(view) == TextOffset{0});
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
    const ProjectId outer = runtime.projects().create({.name = "outer",
                                                       .roots = {"file:///work"},
                                                       .discovery_provider = {},
                                                       .discovery_marker = {}});
    const ProjectId inner = runtime.projects().create({.name = "inner",
                                                       .roots = {"file:///work/sub"},
                                                       .discovery_provider = {},
                                                       .discovery_marker = {}});
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
        insert, context, CommandInvocation{.arguments = {std::string("X")}, .prefix = {}});
    REQUIRE(result.has_value());
    CHECK(runtime.buffers().get(buffer).snapshot().content() == "Xab");
    CHECK(runtime.views().caret(first).value == 1);
    CHECK(runtime.views().caret(second).value == 3);

    runtime.buffers().get(buffer).set_read_only(true);
    CHECK_THROWS_AS(
        [&] {
            (void)runtime.commands().invoke(
                insert, context,
                CommandInvocation{.arguments = {std::string("forbidden")}, .prefix = {}});
        }(),
        std::logic_error);
}

TEST_CASE("command completion owns selection results and strategy edit defaults") {
    EditorRuntime runtime;
    const InputStateId state =
        runtime.input_states().define({.name = "selection-state",
                                       .keymaps = {},
                                       .text_input = TextInputPolicy::Accept,
                                       .text_command = std::string("edit.self-insert"),
                                       .cursor = CursorShape::Beam,
                                       .indicator = {},
                                       .handler = {},
                                       .position_hints = {},
                                       .on_enter = {},
                                       .on_exit = {}});
    const InputStrategyId collapse =
        runtime.input_strategies().define({.name = "selection-collapse",
                                           .editing = state,
                                           .interface = state,
                                           .selection_after_edit = SelectionEditPolicy::Collapse});
    const InputStrategyId preserve =
        runtime.input_strategies().define({.name = "selection-preserve",
                                           .editing = state,
                                           .interface = state,
                                           .selection_after_edit = SelectionEditPolicy::Preserve});
    runtime.set_default_input_strategy(collapse);

    const BufferId buffer = runtime.buffers().create({.name = "selection-command",
                                                      .initial_text = "abcdef",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    CommandLoop loop(runtime);

    const ViewSelection original{.ranges = {{.anchor = TextOffset{1},
                                             .head = TextOffset{3},
                                             .granularity = SelectionGranularity::Character},
                                            {.anchor = TextOffset{5},
                                             .head = TextOffset{4},
                                             .granularity = SelectionGranularity::Node}},
                                 .primary = 0,
                                 .metadata = "(thing . expression)"};
    runtime.views().set_selection(view, original);

    const CommandId no_edit = runtime.commands().define(
        "selection.no-edit", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    CHECK(loop.execute(no_edit, context).status == CommandLoopStatus::Executed);
    CHECK(runtime.views().active_selection(view) == original);

    const auto insert_at_start = [](CommandContext& command_context, std::string_view text) {
        EditTransaction transaction = command_context.buffer().begin_transaction();
        transaction.insert(TextOffset{0}, text);
        (void)transaction.commit();
    };
    const CommandId use_default =
        runtime.commands().define("selection.default",
                                  [insert_at_start](CommandContext& command_context,
                                                    const CommandInvocation&) -> CommandResult {
                                      insert_at_start(command_context, "x");
                                      return CommandCompleted{};
                                  });
    CHECK(loop.execute(use_default, context).status == CommandLoopStatus::Executed);
    CHECK_FALSE(runtime.views().active_selection(view).has_value());

    runtime.views().set_selection(view, original);
    const CommandId preserve_result = runtime.commands().define(
        "selection.preserve",
        [insert_at_start](CommandContext& command_context,
                          const CommandInvocation&) -> CommandResult {
            insert_at_start(command_context, "y");
            return CommandCompleted{.value = std::nullopt, .selection = CommandSelectionPreserve{}};
        });
    CHECK(loop.execute(preserve_result, context).status == CommandLoopStatus::Executed);
    const ViewSelection settled{.ranges = {{.anchor = TextOffset{2},
                                            .head = TextOffset{4},
                                            .granularity = SelectionGranularity::Character},
                                           {.anchor = TextOffset{6},
                                            .head = TextOffset{5},
                                            .granularity = SelectionGranularity::Node}},
                                .primary = 0,
                                .metadata = "(thing . expression)"};
    CHECK(runtime.views().active_selection(view) == settled);

    const ViewSelection replacement{.ranges = {{.anchor = TextOffset{0},
                                                .head = TextOffset{2},
                                                .granularity = SelectionGranularity::Line}},
                                    .primary = 0,
                                    .metadata = "(result . verb)"};
    const CommandId replace_result = runtime.commands().define(
        "selection.replace",
        [replacement](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{.value = std::nullopt, .selection = replacement};
        });
    CHECK(loop.execute(replace_result, context).status == CommandLoopStatus::Executed);
    CHECK(runtime.views().active_selection(view) == replacement);

    runtime.views().set_input_strategy(view, preserve);
    runtime.views().set_selection(view, original);
    CHECK(loop.execute(use_default, context).status == CommandLoopStatus::Executed);
    CHECK(runtime.views().active_selection(view) == settled);
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
            received_repeat = invocation.prefix.count;
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

TEST_CASE("command loop pending prefixes survive layer refresh and flow through dispatch chains") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "prefix",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);

    const CommandPrefix expected{.count = 7,
                                 .register_name = "a",
                                 .extra = {{.name = "scope", .value = std::string("word")},
                                           {.name = "flag", .value = true}}};
    const CommandId set_prefix = runtime.commands().define(
        "prefix.set", [expected](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandPrefixUpdate{.prefix = expected};
        });
    CommandPrefix received;
    const CommandId target = runtime.commands().define(
        "prefix.target",
        [&](CommandContext&, const CommandInvocation& invocation) -> CommandResult {
            received = invocation.prefix;
            return CommandCompleted{};
        });
    const CommandId relay = runtime.commands().define(
        "prefix.relay", [target](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandDispatch{.command = target,
                                   .invocation = {.arguments = {}, .prefix = {}}};
        });
    const KeymapId keymap = runtime.keymaps().define("prefix.map");
    runtime.keymaps().bind(keymap, "p", set_prefix);
    runtime.keymaps().bind(keymap, "r", relay);

    CommandLoop loop(runtime);
    const std::vector<KeymapLayer> layers{{.keymap = keymap, .scope = "test"}};
    loop.set_keymap_layers(layers);
    const KeyStroke prefix_key = parse_key_sequence("p")->front();
    const KeyStroke relay_key = parse_key_sequence("r")->front();
    const CommandLoopResult prefix_result = loop.dispatch(prefix_key, context);
    CHECK(prefix_result.status == CommandLoopStatus::PrefixArgument);
    CHECK(prefix_result.message == "7 \"a scope=word flag=true");
    CHECK(loop.pending_prefix() == expected);

    loop.set_keymap_layers(layers);
    CHECK(loop.pending_prefix() == expected);
    CHECK(loop.dispatch(relay_key, context).status == CommandLoopStatus::Executed);
    CHECK(received == expected);
    CHECK(loop.pending_prefix().empty());

    CHECK(loop.dispatch(prefix_key, context).status == CommandLoopStatus::PrefixArgument);
    CHECK(loop.dispatch(parse_key_sequence("z")->front(), context).status ==
          CommandLoopStatus::NotHandled);
    CHECK(loop.pending_prefix().empty());
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
    std::vector<std::string> lifecycle;
    EditorRuntime runtime;
    const KeymapId normal_map = runtime.keymaps().define("state.normal");
    const KeymapId transient_map = runtime.keymaps().define("state.transient");
    const auto entered = [&](std::string name) {
        return [&, name = std::move(name)](const InputStateChange&) {
            lifecycle.push_back("enter:" + name);
        };
    };
    const auto exited = [&](std::string name) {
        return [&, name = std::move(name)](const InputStateChange&) {
            lifecycle.push_back("exit:" + name);
        };
    };
    const InputStateId emacs =
        runtime.input_states().define({.name = "emacs",
                                       .keymaps = {},
                                       .text_input = TextInputPolicy::Accept,
                                       .text_command = std::string("edit.self-insert"),
                                       .cursor = CursorShape::Beam,
                                       .indicator = "E",
                                       .handler = {},
                                       .position_hints = {},
                                       .on_enter = entered("emacs"),
                                       .on_exit = exited("emacs")});
    const InputStateId normal =
        runtime.input_states().define({.name = "normal",
                                       .keymaps = {normal_map},
                                       .text_input = TextInputPolicy::Ignore,
                                       .text_command = std::nullopt,
                                       .cursor = CursorShape::Block,
                                       .indicator = "N",
                                       .handler = {},
                                       .position_hints = {},
                                       .on_enter = entered("normal"),
                                       .on_exit = exited("normal")});
    const InputStateId transient =
        runtime.input_states().define({.name = "transient",
                                       .keymaps = {transient_map},
                                       .text_input = TextInputPolicy::Ignore,
                                       .text_command = std::nullopt,
                                       .cursor = CursorShape::Underline,
                                       .indicator = "K",
                                       .handler = {},
                                       .position_hints = {},
                                       .on_enter = entered("transient"),
                                       .on_exit = exited("transient")});

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
    CHECK(lifecycle == std::vector<std::string>{"enter:emacs", "enter:normal", "enter:transient",
                                                "exit:emacs", "enter:normal"});

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
    lifecycle.clear();
    REQUIRE(runtime.views().erase(disposable));
    REQUIRE(changes.size() == 1);
    CHECK(changes.front() == InputStateChange{.view = disposable,
                                              .kind = InputStateChangeKind::Pop,
                                              .from = transient,
                                              .to = normal});
    CHECK(lifecycle == std::vector<std::string>{"exit:transient", "exit:normal"});

    CHECK(runtime.input_states().unsubscribe(listener));
    CHECK_FALSE(runtime.input_states().unsubscribe(listener));
    CHECK_THROWS_AS(runtime.views().push_input_state(left, InputStateId{}), std::out_of_range);
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

    const CommandId accept = runtime.commands().define(
        "prompt.accept", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    const CommandId start = runtime.commands().define(
        "prompt.start", [accept](CommandContext&, const CommandInvocation&) -> CommandResult {
            return InteractionRequest{.kind = InteractionKind::Text,
                                      .keymap = "interaction-test",
                                      .input_state = "interaction-input-test",
                                      .buffer_name = " *test-minibuffer*",
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
    const std::size_t initial_buffers = runtime.buffers().all().size();
    const std::size_t initial_windows = runtime.windows().all().size();
    const KeymapId interaction_keymap = runtime.keymaps().define("interaction-test");
    const InputStateId interaction_input =
        define_test_input_state(runtime, "interaction-input-test");
    InteractionMechanisms interaction(runtime, runtime.interaction_providers());
    REQUIRE(interaction.start(*started.interaction, context).has_value());
    REQUIRE(interaction.state() != nullptr);
    const BufferId minibuffer = interaction.state()->buffer;
    const ViewId minibuffer_view = interaction.state()->view;
    const WindowId minibuffer_window = interaction.state()->window;
    CHECK(runtime.buffers().get(minibuffer).kind() == BufferKind::Minibuffer);
    CHECK(runtime.buffers().get(minibuffer).name() == " *test-minibuffer*");
    CHECK(runtime.buffers().all().size() == initial_buffers + 1);
    CHECK(runtime.windows().all().size() == initial_windows + 1);
    CHECK(interaction.state()->origin ==
          CommandTarget{.window = window, .buffer = buffer, .view = view});
    CHECK(runtime.views().get(minibuffer_view).keymaps() ==
          std::vector<KeymapId>{interaction_keymap});
    CHECK(runtime.views().get(minibuffer_view).input_states().top() == interaction_input);
    CHECK(interaction.input_text() == "needle");
    CHECK(interaction.input_caret() == TextOffset{6});

    insert_interaction_text(runtime, interaction, "é");
    EditTransaction erase = runtime.buffers().get(interaction.state()->buffer).begin_transaction();
    erase.erase({TextOffset{6}, TextOffset{8}});
    (void)erase.commit();
    CHECK(interaction.input_text() == "needle");
    CHECK(interaction.input_caret() == TextOffset{6});
    insert_interaction_text(runtime, interaction, "-next");
    const std::expected<std::string, std::string> submission =
        interaction.submit(std::nullopt, true);
    REQUIRE(submission.has_value());
    CHECK(*submission == "needle-next");
    CHECK(runtime.buffers().try_get(minibuffer) == nullptr);
    CHECK(runtime.views().try_get(minibuffer_view) == nullptr);
    CHECK(runtime.windows().try_get(minibuffer_window) == nullptr);
    CHECK(runtime.buffers().all().size() == initial_buffers);
    CHECK(runtime.windows().all().size() == initial_windows);
    CHECK_FALSE(interaction.active());
}

TEST_CASE("interaction input replacement is a native minibuffer text mechanism") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "history-origin",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    const CommandId accept = runtime.commands().define(
        "history.accept", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    (void)runtime.keymaps().define("history-interaction-test");
    (void)define_test_input_state(runtime, "history-interaction-input-test");
    InteractionMechanisms interaction(runtime, runtime.interaction_providers());
    const auto request = [&](std::string initial_input) {
        return InteractionRequest{.kind = InteractionKind::Text,
                                  .keymap = "history-interaction-test",
                                  .input_state = "history-interaction-input-test",
                                  .buffer_name = " *history-minibuffer*",
                                  .prompt = "History: ",
                                  .initial_input = std::move(initial_input),
                                  .history = "commands",
                                  .provider = {},
                                  .allow_custom_input = true,
                                  .accept_command = accept,
                                  .arguments = {}};
    };

    REQUIRE(interaction.start(request("draft"), context).has_value());

    const std::expected<RevisionId, std::string> second = interaction.replace_input("second");
    REQUIRE(second.has_value());
    CHECK(interaction.input_text() == "second");
    CHECK(interaction.input_caret() == TextOffset{6});
    const std::expected<RevisionId, std::string> first = interaction.replace_input("first");
    REQUIRE(first.has_value());
    CHECK(*first > *second);
    CHECK(interaction.input_text() == "first");
    REQUIRE(interaction.replace_input("draft").has_value());
    CHECK(interaction.input_text() == "draft");
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
    (void)runtime.keymaps().define("async-interaction-test");
    (void)define_test_input_state(runtime, "async-interaction-input-test");
    InteractionMechanisms interaction(runtime, runtime.interaction_providers());
    interaction.attach_async_runtime(async);
    REQUIRE(interaction
                .start({.kind = InteractionKind::Picker,
                        .keymap = "async-interaction-test",
                        .input_state = "async-interaction-input-test",
                        .buffer_name = " *async-minibuffer*",
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

    insert_interaction_text(runtime, interaction, "b");
    REQUIRE(interaction.refresh_candidates("async").has_value());
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

TEST_CASE("callback interaction providers support synchronous completion") {
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
        "callback.accept", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    int cancellations = 0;
    runtime.interaction_providers().define(
        "callback", [&](CommandContext&, std::string_view query) -> InteractionProviderResult {
            return InteractionCandidateAsync{
                .start = [query = std::string(query),
                          &cancellations](InteractionCandidateAsync::Completed completed,
                                          InteractionCandidateAsync::Failed,
                                          InteractionCandidateAsync::Cancelled)
                    -> std::expected<InteractionCandidateAsync::Cancel, std::string> {
                    completed({{.value = "provider-first",
                                .label = "provider-first",
                                .detail = "callback",
                                .filter_text = "unrelated"},
                               {.value = query,
                                .label = query,
                                .detail = "callback",
                                .filter_text = query}});
                    return InteractionCandidateAsync::Cancel{[&cancellations] { ++cancellations; }};
                }};
        });
    (void)runtime.keymaps().define("callback-interaction-test");
    (void)define_test_input_state(runtime, "callback-interaction-input-test");
    InteractionMechanisms interaction(runtime, runtime.interaction_providers());

    REQUIRE(interaction
                .start({.kind = InteractionKind::Picker,
                        .keymap = "callback-interaction-test",
                        .input_state = "callback-interaction-input-test",
                        .buffer_name = " *callback-minibuffer*",
                        .prompt = "callback: ",
                        .initial_input = "value",
                        .history = {},
                        .provider = "callback",
                        .allow_custom_input = false,
                        .accept_command = accept,
                        .arguments = {}},
                       context)
                .has_value());
    REQUIRE(interaction.state() != nullptr);
    CHECK_FALSE(interaction.state()->loading);
    REQUIRE(interaction.state()->candidates.size() == 2);
    CHECK(interaction.state()->candidates.front().value == "provider-first");
    CHECK(interaction.cancel());
    CHECK(cancellations == 0);
}

TEST_CASE("async interaction refresh retains candidates until replacement is ready") {
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
        "async-retained.accept", [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
    std::latch replacement_started(1);
    std::latch release_replacement(1);
    runtime.interaction_providers().define(
        "retained", [&](CommandContext&, std::string_view query) -> InteractionProviderResult {
            if (query.empty()) {
                return std::vector<InteractionCandidate>{
                    {.value = "old", .label = "old", .detail = {}, .filter_text = "old"}};
            }
            return InteractionCandidateWork{
                [query = std::string(query), &replacement_started,
                 &release_replacement](const std::stop_token& cancellation) {
                    replacement_started.count_down();
                    release_replacement.wait();
                    if (cancellation.stop_requested()) {
                        throw AsyncTaskCancelled();
                    }
                    return std::vector<InteractionCandidate>{
                        {.value = query, .label = query, .detail = {}, .filter_text = query}};
                }};
        });

    WakeSignal wake;
    AsyncRuntime async([&wake] { wake.notify(); });
    (void)runtime.keymaps().define("retained-interaction-test");
    (void)define_test_input_state(runtime, "retained-interaction-input-test");
    InteractionMechanisms interaction(runtime, runtime.interaction_providers());
    interaction.attach_async_runtime(async);
    REQUIRE(interaction
                .start({.kind = InteractionKind::Picker,
                        .keymap = "retained-interaction-test",
                        .input_state = "retained-interaction-input-test",
                        .buffer_name = " *retained-minibuffer*",
                        .prompt = "async: ",
                        .initial_input = {},
                        .history = {},
                        .provider = "retained",
                        .allow_custom_input = false,
                        .accept_command = accept,
                        .arguments = {}},
                       context)
                .has_value());
    REQUIRE(interaction.state() != nullptr);
    REQUIRE(interaction.state()->candidates.size() == 1);
    CHECK(interaction.state()->candidates.front().value == "old");

    insert_interaction_text(runtime, interaction, "new");
    REQUIRE(interaction.refresh_candidates("retained").has_value());
    replacement_started.wait();
    REQUIRE(interaction.state() != nullptr);
    CHECK(interaction.state()->loading);
    REQUIRE(interaction.state()->candidates.size() == 1);
    CHECK(interaction.state()->candidates.front().value == "old");

    release_replacement.count_down();
    REQUIRE(wake.wait());
    CHECK(async.drain() == 1);
    REQUIRE(interaction.state()->candidates.size() == 1);
    CHECK(interaction.state()->candidates.front().value == "new");
}

TEST_CASE("completion pipeline merges fixed providers and addresses duplicate labels by id") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "completion",
                                                      .initial_text = "fo",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{2});
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    AsyncRuntime async;
    CompletionPipeline pipeline(
        runtime, async,
        [](CompletionProvider provider,
           const CompletionRequest& request) -> CompletionProviderResult {
            const std::string detail = completion_provider_name(provider);
            return CompletionProviderResponse{
                .provider = provider,
                .items = {{.id = provider.kind == CompletionProviderKind::Word ? 11U : 22U,
                           .provider = provider,
                           .filter_text = "foobar",
                           .label = "same label",
                           .kind = "value",
                           .detail = detail,
                           .edit = CompletionEdit{.insert_range = {request.anchor, request.caret},
                                                  .replace_range = {request.anchor, request.caret},
                                                  .new_text = detail},
                           .sort_text = detail,
                           .resolved = true,
                           .resolving = false,
                           .resolve_error = {},
                           .documentation = {},
                           .additional_edits = {},
                           .raw = {}}},
                .is_incomplete = false};
        });

    REQUIRE(
        pipeline
            .start(context, TextOffset{}, {CompletionProvider::word(), CompletionProvider::path()})
            .has_value());
    REQUIRE(pipeline.state() != nullptr);
    REQUIRE(pipeline.state()->matches.size() == 2);
    CHECK(pipeline.state()->matches[0].item.label == "same label");
    CHECK(pipeline.state()->matches[1].item.label == "same label");
    CHECK(pipeline.state()->matches[0].item.id != pipeline.state()->matches[1].item.id);
    CHECK(pipeline.state()->providers.size() == 2);
}

TEST_CASE("completion pipeline resolves visible candidates in place by stable id") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "completion-resolve",
                                                      .initial_text = "fo",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{2});
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    AsyncRuntime async;
    int resolve_requests = 0;
    CompletionPipeline pipeline(
        runtime, async,
        [](CompletionProvider provider,
           const CompletionRequest& request) -> CompletionProviderResult {
            std::vector<CompletionItem> items;
            for (const std::uint64_t id : {41U, 42U}) {
                const std::string label = std::format("foo{}", id);
                items.push_back(
                    {.id = id,
                     .provider = provider,
                     .filter_text = label,
                     .label = label,
                     .kind = "function",
                     .detail = "unresolved",
                     .edit = CompletionEdit{.insert_range = {request.anchor, request.caret},
                                            .replace_range = {request.anchor, request.caret},
                                            .new_text = label},
                     .sort_text = label,
                     .resolved = false,
                     .resolving = false,
                     .resolve_error = {},
                     .documentation = {},
                     .additional_edits = {},
                     .raw = std::format("{{\"id\":{}}}", id)});
            }
            return CompletionProviderResponse{
                .provider = provider, .items = std::move(items), .is_incomplete = false};
        },
        [&](const CompletionRequest&, const CompletionItem& item,
            CompletionPipeline::ResolveCompleted completed, CompletionProviderAsync::Failed,
            CompletionProviderAsync::Cancelled)
            -> std::expected<CompletionProviderAsync::Cancel, std::string> {
            ++resolve_requests;
            CompletionItem resolved = item;
            resolved.detail = "resolved";
            resolved.documentation = std::format("documentation for {}", item.label);
            resolved.resolved = true;
            completed(std::move(resolved));
            return CompletionProviderAsync::Cancel{};
        });

    REQUIRE(pipeline.start(context, TextOffset{}, {CompletionProvider::lsp(1)}).has_value());
    REQUIRE(pipeline.state() != nullptr);
    CHECK(resolve_requests == 1);
    REQUIRE(pipeline.state()->matches.size() == 2);
    const std::uint64_t selected_id = pipeline.state()->matches.front().item.id;
    CHECK(pipeline.state()->matches.front().item.resolved);
    CHECK(pipeline.state()->matches.front().item.documentation ==
          std::format("documentation for {}", pipeline.state()->matches.front().item.label));

    pipeline.resolve_visible(0, 2);
    CHECK(resolve_requests == 2);
    REQUIRE(pipeline.state() != nullptr);
    CHECK(pipeline.state()->matches.front().item.id == selected_id);
    CHECK(std::ranges::all_of(pipeline.state()->matches, [](const CompletionMatch& match) {
        return match.item.resolved && match.item.detail == "resolved";
    }));
}

TEST_CASE("completion pipeline cancels outstanding item resolution with its session") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "completion-resolve-cancel",
                                                      .initial_text = "f",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{1});
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    AsyncRuntime async;
    int cancellations = 0;
    CompletionPipeline pipeline(
        runtime, async,
        [](CompletionProvider provider,
           const CompletionRequest& request) -> CompletionProviderResult {
            return CompletionProviderResponse{
                .provider = provider,
                .items = {{.id = 9,
                           .provider = provider,
                           .filter_text = "foo",
                           .label = "foo",
                           .kind = "function",
                           .detail = {},
                           .edit = CompletionEdit{.insert_range = {request.anchor, request.caret},
                                                  .replace_range = {request.anchor, request.caret},
                                                  .new_text = "foo"},
                           .sort_text = {},
                           .resolved = false,
                           .resolving = false,
                           .resolve_error = {},
                           .documentation = {},
                           .additional_edits = {},
                           .raw = "payload"}},
                .is_incomplete = false};
        },
        [&](const CompletionRequest&, const CompletionItem&, CompletionPipeline::ResolveCompleted,
            CompletionProviderAsync::Failed, CompletionProviderAsync::Cancelled)
            -> std::expected<CompletionProviderAsync::Cancel, std::string> {
            return CompletionProviderAsync::Cancel{[&] { ++cancellations; }};
        });

    REQUIRE(pipeline.start(context, TextOffset{}, {CompletionProvider::lsp(1)}).has_value());
    REQUIRE(pipeline.state() != nullptr);
    CHECK(pipeline.state()->matches.front().item.resolving);
    CHECK(pipeline.cancel());
    CHECK(cancellations == 1);
}

TEST_CASE("completion application waits for resolve and commits the resolved edits") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "completion-deferred-apply",
                                                      .initial_text = "fo\n",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{2});
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    AsyncRuntime async;
    CompletionPipeline::ResolveCompleted resolve_completed;
    CompletionItem unresolved;
    int applied = 0;
    CompletionPipeline pipeline(
        runtime, async,
        [](CompletionProvider provider,
           const CompletionRequest& request) -> CompletionProviderResult {
            return CompletionProviderResponse{
                .provider = provider,
                .items = {{.id = 17,
                           .provider = provider,
                           .filter_text = "format",
                           .label = "format",
                           .kind = "function",
                           .detail = {},
                           .edit = CompletionEdit{.insert_range = {request.anchor, request.caret},
                                                  .replace_range = {request.anchor, request.caret},
                                                  .new_text = "format"},
                           .sort_text = {},
                           .resolved = false,
                           .resolving = false,
                           .resolve_error = {},
                           .documentation = {},
                           .additional_edits = {},
                           .raw = "payload"}},
                .is_incomplete = false};
        },
        [&](const CompletionRequest&, const CompletionItem& item,
            CompletionPipeline::ResolveCompleted completed, CompletionProviderAsync::Failed,
            CompletionProviderAsync::Cancelled)
            -> std::expected<CompletionProviderAsync::Cancel, std::string> {
            unresolved = item;
            resolve_completed = std::move(completed);
            return CompletionProviderAsync::Cancel{[] {}};
        },
        [&](CommandTarget target) {
            CHECK(target.window == window);
            CHECK(target.buffer == buffer);
            CHECK(target.view == view);
            ++applied;
        });

    REQUIRE(pipeline.start(context, TextOffset{}, {CompletionProvider::lsp(1)}).has_value());
    REQUIRE(resolve_completed);
    REQUIRE(pipeline.apply().has_value());
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() == "fo\n");
    CHECK(applied == 0);

    unresolved.resolved = true;
    unresolved.resolving = false;
    unresolved.additional_edits = {{{TextOffset{3}, TextOffset{3}}, "#include <format>\n"}};
    resolve_completed(std::move(unresolved));
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() ==
          "format\n#include <format>\n");
    CHECK(applied == 1);
    CHECK_FALSE(pipeline.active());
}

TEST_CASE("completion pipeline refilters complete sources and requeries only incomplete sources") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "completion-update",
                                                      .initial_text = "f",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{1});
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    AsyncRuntime async;
    int word_requests = 0;
    int path_requests = 0;
    CompletionPipeline pipeline(
        runtime, async,
        [&](CompletionProvider provider,
            const CompletionRequest& request) -> CompletionProviderResult {
            const bool word = provider.kind == CompletionProviderKind::Word;
            (word ? word_requests : path_requests) += 1;
            return CompletionProviderResponse{
                .provider = provider,
                .items = {{.id = word ? 1U : static_cast<std::uint64_t>(path_requests + 10),
                           .provider = provider,
                           .filter_text = word ? "first" : "foo.hpp",
                           .label = word ? "first" : "foo.hpp",
                           .kind = word ? "word" : "path",
                           .detail = {},
                           .edit = CompletionEdit{.insert_range = {request.anchor, request.caret},
                                                  .replace_range = {request.anchor, request.caret},
                                                  .new_text = word ? "first" : "foo.hpp"},
                           .sort_text = {},
                           .resolved = true,
                           .resolving = false,
                           .resolve_error = {},
                           .documentation = {},
                           .additional_edits = {},
                           .raw = {}}},
                .is_incomplete = !word};
        });
    REQUIRE(
        pipeline
            .start(context, TextOffset{}, {CompletionProvider::word(), CompletionProvider::path()})
            .has_value());
    CHECK(word_requests == 1);
    CHECK(path_requests == 1);

    EditTransaction transaction = runtime.buffers().get(buffer).begin_transaction();
    transaction.insert(TextOffset{1}, "o");
    (void)transaction.commit();
    runtime.views().set_caret(view, TextOffset{2});
    REQUIRE(pipeline.update(context).has_value());
    CHECK(word_requests == 1);
    CHECK(path_requests == 2);
    REQUIRE(pipeline.state() != nullptr);
    CHECK(pipeline.state()->request.query == "fo");
    REQUIRE(pipeline.state()->matches.size() == 1);
    CHECK(pipeline.state()->matches.front().item.label == "foo.hpp");
}

TEST_CASE("completion application merges primary and additional edits into one undo step") {
    EditorRuntime runtime;
    const std::string original = "#include\nfo";
    const BufferId buffer = runtime.buffers().create({.name = "completion-apply",
                                                      .initial_text = original,
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{11});
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    AsyncRuntime async;
    CompletionPipeline pipeline(
        runtime, async,
        [](CompletionProvider provider,
           const CompletionRequest& request) -> CompletionProviderResult {
            return CompletionProviderResponse{
                .provider = provider,
                .items = {{.id = 1,
                           .provider = provider,
                           .filter_text = "foo",
                           .label = "foo",
                           .kind = "function",
                           .detail = {},
                           .edit = CompletionEdit{.insert_range = {request.anchor, request.caret},
                                                  .replace_range = {request.anchor, request.caret},
                                                  .new_text = "foo"},
                           .sort_text = {},
                           .resolved = true,
                           .resolving = false,
                           .resolve_error = {},
                           .documentation = {},
                           .additional_edits = {{{TextOffset{}, TextOffset{}}, "using X;\n"}},
                           .raw = {}}},
                .is_incomplete = false};
        });
    REQUIRE(pipeline.start(context, TextOffset{9}, {CompletionProvider::word()}).has_value());
    REQUIRE(pipeline.apply().has_value());
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() ==
          "using X;\n#include\nfoo");
    CHECK(runtime.views().caret(view) == TextOffset{21});
    REQUIRE(runtime.buffers().get(buffer).undo().has_value());
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() == original);
}
