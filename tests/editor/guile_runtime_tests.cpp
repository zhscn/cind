#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/runtime.hpp"
#include "script/guile_runtime.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

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

    const BufferId buffer = runtime.buffers().create({.name = "sample",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const BufferId other = runtime.buffers().create({.name = "other",
                                                     .initial_text = {},
                                                     .kind = BufferKind::Scratch,
                                                     .resource_uri = std::nullopt,
                                                     .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);

    std::pair<WindowId, BufferId> displayed;
    bool buffer_displayed = false;
    std::tuple<ViewId, std::uint32_t, std::uint32_t> moved;
    bool caret_moved = false;
    std::string message;
    ProjectId indexed_project;
    bool project_index_requested = false;
    WindowId opened_window;
    std::string opened_path;
    bool file_opened = false;
    ProjectId searched_project;
    WindowId searched_window;
    std::string search_query;
    bool project_search_started = false;
    BufferId resource_buffer;
    std::string resource_path;
    bool buffer_resource_set = false;
    GuileRuntime guile(
        runtime,
        {.display_buffer = [&](WindowId target_window,
                               BufferId target_buffer) -> std::expected<void, std::string> {
             displayed = std::pair{target_window, target_buffer};
             buffer_displayed = true;
             return {};
         },
         .move_caret_to_line = [&](ViewId target_view, std::uint32_t line,
                                   std::uint32_t column) -> std::expected<void, std::string> {
             moved = std::tuple{target_view, line, column};
             caret_moved = true;
             return {};
         },
         .set_message = [&](std::string value) { message = std::move(value); },
         .ensure_project_index = [&](ProjectId target) -> std::expected<void, std::string> {
             indexed_project = target;
             project_index_requested = true;
             return {};
         },
         .open_file = [&](WindowId target, std::string path) -> std::expected<void, std::string> {
             opened_window = target;
             opened_path = std::move(path);
             file_opened = true;
             return {};
         },
         .start_project_search = [&](ProjectId target_project, WindowId target_window,
                                     std::string query) -> std::expected<void, std::string> {
             searched_project = target_project;
             searched_window = target_window;
             search_query = std::move(query);
             project_search_started = true;
             return {};
         },
         .set_buffer_resource = [&](BufferId target_buffer,
                                    std::string path) -> std::expected<void, std::string> {
             resource_buffer = target_buffer;
             resource_path = std::move(path);
             buffer_resource_set = true;
             return {};
         }});
    const std::expected<std::size_t, std::string> installed = guile.install_core_commands();
    REQUIRE(installed.has_value());
    CHECK(*installed == 16);

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

    const CommandResult open_result =
        runtime.commands().invoke(require_command(runtime, "file.open"), context);
    REQUIRE(open_result.has_value());
    const auto* open_request = std::get_if<InteractionRequest>(&*open_result);
    REQUIRE(open_request != nullptr);
    CHECK(open_request->kind == InteractionKind::Picker);
    CHECK(open_request->prompt == "Open file: ");
    CHECK(open_request->initial_input ==
          std::string(".") + std::filesystem::path::preferred_separator);
    CHECK(open_request->provider == "files");
    CHECK(runtime.commands().definition(open_request->accept_command).name == "file.open.accept");

    const std::string directory = std::string("/tmp") + std::filesystem::path::preferred_separator;
    const CommandResult directory_result = runtime.commands().invoke(
        open_request->accept_command, context,
        CommandInvocation{.arguments = {directory}, .repeat_count = std::nullopt});
    REQUIRE(directory_result.has_value());
    const auto* directory_request = std::get_if<InteractionRequest>(&*directory_result);
    REQUIRE(directory_request != nullptr);
    CHECK(directory_request->initial_input == directory);
    CHECK_FALSE(file_opened);

    const CommandResult opened =
        runtime.commands().invoke(open_request->accept_command, context,
                                  CommandInvocation{.arguments = {std::string("/tmp/example.cpp")},
                                                    .repeat_count = std::nullopt});
    REQUIRE(opened.has_value());
    REQUIRE(file_opened);
    CHECK(opened_window == window);
    CHECK(opened_path == "/tmp/example.cpp");

    const CommandResult save_as_result =
        runtime.commands().invoke(require_command(runtime, "file.save-as"), context);
    REQUIRE(save_as_result.has_value());
    const auto* save_as_request = std::get_if<InteractionRequest>(&*save_as_result);
    REQUIRE(save_as_request != nullptr);
    CHECK(save_as_request->kind == InteractionKind::Text);
    CHECK(save_as_request->prompt == "Write file: ");
    CHECK(save_as_request->initial_input.empty());
    CHECK(runtime.commands().definition(save_as_request->accept_command).name ==
          "file.save-as.accept");
    const CommandResult save_as_accepted =
        runtime.commands().invoke(save_as_request->accept_command, context,
                                  CommandInvocation{.arguments = {std::string("/tmp/written.cpp")},
                                                    .repeat_count = std::nullopt});
    REQUIRE(save_as_accepted.has_value());
    REQUIRE(buffer_resource_set);
    CHECK(resource_buffer == buffer);
    CHECK(resource_path == "/tmp/written.cpp");
    const auto* save_dispatch = std::get_if<CommandDispatch>(&*save_as_accepted);
    REQUIRE(save_dispatch != nullptr);
    CHECK(save_dispatch->command == save);

    const CommandResult switched = runtime.commands().invoke(
        require_command(runtime, "buffer.switch.accept"), context,
        CommandInvocation{.arguments = {std::string("other")}, .repeat_count = std::nullopt});
    REQUIRE(switched.has_value());
    REQUIRE(buffer_displayed);
    CHECK(displayed.first == window);
    CHECK(displayed.second == other);

    const CommandResult unknown_buffer = runtime.commands().invoke(
        require_command(runtime, "buffer.switch.accept"), context,
        CommandInvocation{.arguments = {std::string("missing")}, .repeat_count = std::nullopt});
    REQUIRE_FALSE(unknown_buffer.has_value());
    CHECK(unknown_buffer.error().message == "unknown buffer 'missing'");

    const CommandResult moved_to_line = runtime.commands().invoke(
        require_command(runtime, "cursor.goto-line.accept"), context,
        CommandInvocation{.arguments = {std::string("4:7")}, .repeat_count = std::nullopt});
    REQUIRE(moved_to_line.has_value());
    REQUIRE(caret_moved);
    CHECK(std::get<0>(moved) == view);
    CHECK(std::get<1>(moved) == 3);
    CHECK(std::get<2>(moved) == 6);

    const CommandResult invalid_line = runtime.commands().invoke(
        require_command(runtime, "cursor.goto-line.accept"), context,
        CommandInvocation{.arguments = {std::string("0")}, .repeat_count = std::nullopt});
    REQUIRE_FALSE(invalid_line.has_value());
    CHECK(invalid_line.error().message == "invalid line number");

    const CommandResult help = runtime.commands().invoke(
        require_command(runtime, "help.keys.accept"), context,
        CommandInvocation{.arguments = {std::string("C-x C-s  file.save")},
                          .repeat_count = std::nullopt});
    REQUIRE(help.has_value());
    CHECK(message == "C-x C-s  file.save");

    const CommandId project_search = require_command(runtime, "project.search");
    CHECK_FALSE(runtime.commands().enabled(project_search, context));
    const ProjectId project =
        runtime.projects().create({.name = "sample", .roots = {"/tmp/sample"}});
    runtime.projects().assign(buffer, project);
    CHECK(runtime.commands().enabled(project_search, context));

    const CommandId project_find_file = require_command(runtime, "project.find-file");
    CHECK(runtime.commands().enabled(project_find_file, context));
    const CommandResult find_file_result = runtime.commands().invoke(project_find_file, context);
    REQUIRE(find_file_result.has_value());
    const auto* find_file_request = std::get_if<InteractionRequest>(&*find_file_result);
    REQUIRE(find_file_request != nullptr);
    CHECK(find_file_request->kind == InteractionKind::Picker);
    CHECK(find_file_request->prompt == "Project file: ");
    CHECK(find_file_request->provider == "project-files");
    CHECK(runtime.commands().definition(find_file_request->accept_command).name ==
          "project.find-file.accept");
    REQUIRE(project_index_requested);
    CHECK(indexed_project == project);

    file_opened = false;
    opened_path.clear();
    const CommandResult file_accepted = runtime.commands().invoke(
        find_file_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("/tmp/sample/main.cpp")},
                          .repeat_count = std::nullopt});
    REQUIRE(file_accepted.has_value());
    REQUIRE(file_opened);
    CHECK(opened_window == window);
    CHECK(opened_path == "/tmp/sample/main.cpp");

    const CommandResult search_result = runtime.commands().invoke(project_search, context);
    REQUIRE(search_result.has_value());
    const auto* search_request = std::get_if<InteractionRequest>(&*search_result);
    REQUIRE(search_request != nullptr);
    CHECK(runtime.commands().definition(search_request->accept_command).name ==
          "project.search.accept");
    const CommandResult empty_search = runtime.commands().invoke(
        search_request->accept_command, context,
        CommandInvocation{.arguments = {std::string()}, .repeat_count = std::nullopt});
    REQUIRE_FALSE(empty_search.has_value());
    CHECK(empty_search.error().message == "project search query is empty");
    const CommandResult search_accepted = runtime.commands().invoke(
        search_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("needle")}, .repeat_count = std::nullopt});
    REQUIRE(search_accepted.has_value());
    REQUIRE(project_search_started);
    CHECK(searched_project == project);
    CHECK(searched_window == window);
    CHECK(search_query == "needle");

    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.command_revision == 1);
    CHECK(snapshot.scripted_commands == 16);
    CHECK_FALSE(snapshot.last_error.has_value());
}
