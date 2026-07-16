#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/runtime.hpp"
#include "script/guile_runtime.hpp"

#include <string>
#include <string_view>

using namespace cind;

namespace {

CommandId define_command(EditorRuntime& runtime, std::string name) {
    return runtime.commands().define(
        std::move(name), [](CommandContext&, const CommandInvocation&) -> CommandResult {
            return CommandCompleted{};
        });
}

CommandId resolve_command(const EditorRuntime& runtime, KeymapId keymap, std::string_view keys) {
    const std::expected<KeySequence, KeyParseError> sequence = parse_key_sequence(keys);
    REQUIRE(sequence.has_value());
    const KeymapMatch match = runtime.keymaps().resolve(keymap, *sequence);
    REQUIRE(match.kind == KeymapMatchKind::Command);
    return match.command;
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

TEST_CASE("bundled Guile policy installs available default key bindings") {
    EditorRuntime runtime;
    const KeymapId editor = runtime.keymaps().define("editor.default");
    const KeymapId application = runtime.keymaps().define("application.global");
    const CommandId save = define_command(runtime, "file.save");
    const CommandId replace = define_command(runtime, "search.replace");
    const CommandId quit = define_command(runtime, "application.quit");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> installed = guile.install_default_keymaps();

    REQUIRE(installed.has_value());
    CHECK(*installed == 3);
    CHECK(resolve_command(runtime, editor, "C-x C-s") == save);
    CHECK(resolve_command(runtime, editor, "M-%") == replace);
    CHECK(resolve_command(runtime, application, "C-x C-c") == quit);

    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.engine == "guile");
    CHECK_FALSE(snapshot.version.empty());
    CHECK(snapshot.modules == std::vector<std::string>{"cind command", "cind core"});
    CHECK(snapshot.binding_revision == 1);
    CHECK_FALSE(snapshot.last_error.has_value());
}

TEST_CASE("Guile keymap policy treats unavailable commands as optional") {
    EditorRuntime runtime;
    const KeymapId editor = runtime.keymaps().define("editor.default");
    runtime.keymaps().define("application.global");
    const CommandId save = define_command(runtime, "file.save");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> first = guile.install_default_keymaps();
    const std::expected<std::size_t, std::string> second = guile.install_default_keymaps();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 1);
    CHECK(*second == 1);
    CHECK(resolve_command(runtime, editor, "C-x C-s") == save);
    CHECK(guile.snapshot().binding_revision == 2);
}

TEST_CASE("bundled Guile commands return editor command actions") {
    EditorRuntime runtime;
    const CommandId save = define_command(runtime, "file.save");
    (void)define_command(runtime, "buffer.switch.accept");
    (void)define_command(runtime, "cursor.goto-line.accept");
    (void)define_command(runtime, "project.search.accept");
    (void)define_command(runtime, "help.keys.accept");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> installed = guile.install_core_commands();
    REQUIRE(installed.has_value());
    CHECK(*installed == 6);

    const BufferId buffer = runtime.buffers().create({.name = "sample",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);

    const CommandId palette = require_command(runtime, "command.palette");
    const CommandResult palette_result = runtime.commands().invoke(palette, context);
    REQUIRE(palette_result.has_value());
    const auto* request = std::get_if<InteractionRequest>(&*palette_result);
    REQUIRE(request != nullptr);
    CHECK(request->kind == InteractionKind::Picker);
    CHECK(request->prompt == "Command: ");
    CHECK(request->provider == "commands");
    CHECK(runtime.commands().definition(request->accept_command).name == "command.palette.accept");

    const CommandResult missing = runtime.commands().invoke(request->accept_command, context);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message == "command palette requires a command name");

    const CommandResult accepted = runtime.commands().invoke(
        request->accept_command, context,
        CommandInvocation{.arguments = {std::string("file.save")}, .repeat_count = std::nullopt});
    REQUIRE(accepted.has_value());
    const auto* dispatch = std::get_if<CommandDispatch>(&*accepted);
    REQUIRE(dispatch != nullptr);
    CHECK(dispatch->command == save);

    const CommandId project_search = require_command(runtime, "project.search");
    CHECK_FALSE(runtime.commands().enabled(project_search, context));
    const ProjectId project =
        runtime.projects().create({.name = "sample", .roots = {"/tmp/sample"}});
    runtime.projects().assign(buffer, project);
    CHECK(runtime.commands().enabled(project_search, context));

    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.command_revision == 1);
    CHECK(snapshot.scripted_commands == 6);
    CHECK_FALSE(snapshot.last_error.has_value());
}
