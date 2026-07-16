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
    CHECK(snapshot.modules == std::vector<std::string>{"cind core"});
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
