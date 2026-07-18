#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/command_loop.hpp"
#include "editor/cpp_mode.hpp"
#include "editor/runtime.hpp"
#include "script/guile_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

KeymapId require_keymap(const EditorRuntime& runtime, std::string_view name) {
    const std::optional<KeymapId> keymap = runtime.keymaps().find(name);
    if (!keymap) {
        FAIL("missing keymap: ", name);
        return {};
    }
    return *keymap;
}

std::vector<InteractionCandidate> complete_provider(EditorRuntime& runtime, std::string_view name,
                                                    CommandContext& context,
                                                    std::string_view query = {}) {
    InteractionProviderResult result =
        runtime.interaction_providers().complete(name, context, query);
    auto* candidates = std::get_if<std::vector<InteractionCandidate>>(&result);
    if (candidates == nullptr) {
        FAIL("provider returned asynchronous work: ", name);
        return {};
    }
    return std::move(*candidates);
}

class SchemeFile {
public:
    SchemeFile(std::string name, std::string_view contents)
        : path_(std::filesystem::temp_directory_path() / std::move(name)) {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output) {
            throw std::runtime_error("cannot create temporary Scheme file");
        }
    }

    ~SchemeFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

GuileHostServices async_services(AsyncScriptHost& host) {
    GuileHostServices services;
    services.start_async_task = [&host](ScriptAsyncRequest request,
                                        ScriptAsyncCallbacks callbacks) {
        return host.start(std::move(request), std::move(callbacks));
    };
    services.cancel_async_task = [&host](std::uint64_t task) { return host.cancel(task); };
    services.async_tasks = [&host] { return host.tasks(); };
    return services;
}

template <typename Predicate> void drain_until(AsyncRuntime& runtime, Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        (void)runtime.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)runtime.drain();
    REQUIRE(predicate());
}

} // namespace

TEST_CASE("Guile extensions load in an isolated module") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);
    const SchemeFile extension("cind-guile-extension-success.scm",
                               R"((define private-value 42)
(define-command! host "user.answer"
  (lambda (context invocation) (command-completed private-value))
  #f)
(define-keymap! host 'user.map #f)
(bind-key! host 'user.map "C-c a" 'user.answer)
)");

    const std::expected<void, std::string> loaded = guile.load_extension(extension.path().string());

    REQUIRE(loaded.has_value());
    const CommandId command = require_command(runtime, "user.answer");
    const KeymapId keymap = require_keymap(runtime, "user.map");
    CHECK(resolve_command(runtime, keymap, "C-c a") == command);
    CHECK(runtime.commands().definition(command).source ==
          std::format("scheme:{}", extension.path().string()));
    REQUIRE(guile.install_core_providers().has_value());
    const BufferId buffer = runtime.buffers().create({.name = "extension-test",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    const std::vector<InteractionCandidate> variables =
        complete_provider(runtime, "scheme-variables", context);
    CHECK(std::ranges::any_of(variables, [&](const InteractionCandidate& candidate) {
        return candidate.label == "private-value" && candidate.detail == extension.path().string();
    }));
    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    REQUIRE(snapshot.extensions.size() == 1);
    CHECK(snapshot.extensions.front() == extension.path().string());
    CHECK(snapshot.command_revision == 1);
    CHECK(snapshot.binding_revision == 1);
    CHECK_FALSE(snapshot.last_error.has_value());
}

TEST_CASE("failed Guile extension loads roll back every registration") {
    EditorRuntime runtime;
    (void)runtime.modes().define("text-mode", ModeKind::Major);
    GuileRuntime guile(runtime);
    const SchemeFile extension("cind-guile-extension-failure.scm",
                               R"((define-command! host "user.partial"
  (lambda (context invocation) (command-completed))
  #f)
(define-keymap! host 'user.partial-map #f)
(bind-key! host 'user.partial-map "C-c p" 'user.partial)
(define-file-mode-rule! host 'user.partial-mode 'text-mode '(".partial") '())
(define-project-provider! host 'user.partial-project '("partial.project"))
(error "extension failed")
)");

    const std::expected<void, std::string> loaded = guile.load_extension(extension.path().string());

    REQUIRE_FALSE(loaded.has_value());
    CHECK_FALSE(runtime.commands().find("user.partial").has_value());
    CHECK_FALSE(runtime.keymaps().find("user.partial-map").has_value());
    CHECK(runtime.resource_policies().file_mode_rules().empty());
    CHECK(runtime.resource_policies().project_providers().empty());
    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.extensions.empty());
    REQUIRE(snapshot.last_error.has_value());
    CHECK(snapshot.last_error.value_or("").find("extension failed") != std::string::npos);
}

TEST_CASE("failed Guile extension loads cancel tasks created during evaluation") {
    AsyncRuntime async;
    AsyncScriptHost async_host(async);
    EditorRuntime runtime;
    GuileRuntime guile(runtime, async_services(async_host));
    const SchemeFile extension("cind-guile-extension-async-failure.scm",
                               R"((start-async-task!
 host
 (async-process "/bin/sh" '("-c" "sleep 30"))
 (lambda (task result) #f))
(error "extension failed"))");

    const std::expected<void, std::string> loaded = guile.load_extension(extension.path().string());

    REQUIRE_FALSE(loaded.has_value());
    CHECK(guile.snapshot().outstanding_async_tasks == 0);
    drain_until(async, [&] { return async_host.tasks().empty(); });
}

TEST_CASE("Guile evaluation keeps an application-local module and reports streams") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);

    const std::expected<GuileEvaluationResult, std::string> first =
        guile.evaluate({.source = "(define answer 41)\n(display \"hello\\n\")\n(+ answer 1)\n",
                        .source_name = "scratch.scm"});
    REQUIRE(first.has_value());
    CHECK_FALSE(first->error.has_value());
    CHECK(first->values == std::vector<std::string>{"42"});
    CHECK(first->output == "hello\n");
    CHECK(first->error_output.empty());

    const std::expected<GuileEvaluationResult, std::string> second =
        guile.evaluate({.source = "answer", .source_name = "scratch.scm"});
    REQUIRE(second.has_value());
    CHECK(second->values == std::vector<std::string>{"41"});

    {
        EditorRuntime other_runtime;
        GuileRuntime other(other_runtime);
        const std::expected<GuileEvaluationResult, std::string> isolated =
            other.evaluate({.source = "(defined? 'answer)", .source_name = "other.scm"});
        REQUIRE(isolated.has_value());
        CHECK(isolated->values == std::vector<std::string>{"#f"});
    }

    REQUIRE(guile.install_core_providers().has_value());
    const BufferId buffer = runtime.buffers().create({.name = "evaluation-test",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    const std::vector<InteractionCandidate> variables =
        complete_provider(runtime, "scheme-variables", context);
    CHECK(std::ranges::any_of(variables, [](const InteractionCandidate& candidate) {
        return candidate.label == "answer" && candidate.detail == "*scheme-user*";
    }));

    const std::expected<GuileEvaluationResult, std::string> registration =
        guile.evaluate({.source = R"((define-command! host "user.evaluated"
  (lambda (context invocation) (command-completed answer))
  #f))",
                        .source_name = "scratch.scm"});
    REQUIRE(registration.has_value());
    CHECK_FALSE(registration->error.has_value());
    const CommandId command = require_command(runtime, "user.evaluated");
    CHECK(runtime.commands().definition(command).source == "scheme:scratch.scm");
    CHECK(guile.snapshot().command_revision == 1);

    const std::expected<GuileEvaluationResult, std::string> failed =
        guile.evaluate({.source = "(error \"evaluation failed\")", .source_name = "scratch.scm"});
    REQUIRE(failed.has_value());
    REQUIRE(failed->error.has_value());
    CHECK(failed->error.value_or("").find("evaluation failed") != std::string::npos);
}

TEST_CASE("Guile async tasks deliver typed results and cancellation on the editor thread") {
    AsyncRuntime async;
    AsyncScriptHost async_host(async);
    EditorRuntime runtime;
    GuileRuntime guile(runtime, async_services(async_host));

    const std::expected<GuileEvaluationResult, std::string> started =
        guile.evaluate({.source = R"((define async-value #f)
(start-async-task!
 host
 (async-process "/bin/sh" '("-c" "printf scheme; exit 4"))
 (lambda (task result) (set! async-value result)))
(async-task-summaries host))",
                        .source_name = "async-test.scm"});
    REQUIRE(started.has_value());
    CHECK_FALSE(started->error.has_value());
    CHECK(guile.snapshot().outstanding_async_tasks == 1);
    drain_until(async, [&] { return guile.snapshot().outstanding_async_tasks == 0; });

    const std::expected<GuileEvaluationResult, std::string> completed =
        guile.evaluate({.source = "async-value", .source_name = "async-test.scm"});
    REQUIRE(completed.has_value());
    CHECK(completed->values == std::vector<std::string>{"#(process 4 0 \"scheme\" \"\")"});

    const std::expected<GuileEvaluationResult, std::string> cancellation =
        guile.evaluate({.source = R"((define cancelled-value #f)
(define cancelled-task
  (start-async-task!
   host
   (async-process "/bin/sh" '("-c" "sleep 30"))
   (lambda (task result) (set! cancelled-value 'completed))
   #:cancelled (lambda (task) (set! cancelled-value 'cancelled))))
(cancel-async-task! host cancelled-task))",
                        .source_name = "async-test.scm"});
    REQUIRE(cancellation.has_value());
    CHECK(cancellation->values == std::vector<std::string>{"#t"});
    drain_until(async, [&] { return guile.snapshot().outstanding_async_tasks == 0; });
    const std::expected<GuileEvaluationResult, std::string> cancelled =
        guile.evaluate({.source = "cancelled-value", .source_name = "async-test.scm"});
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->values == std::vector<std::string>{"cancelled"});

    const std::expected<GuileEvaluationResult, std::string> failure =
        guile.evaluate({.source = R"((define failure-value #f)
(start-async-task!
 host
 (async-process "/cind/does/not/exist" '())
 (lambda (task result) (set! failure-value 'completed))
 #:failed (lambda (task message) (set! failure-value message))))",
                        .source_name = "async-test.scm"});
    REQUIRE(failure.has_value());
    drain_until(async, [&] { return guile.snapshot().outstanding_async_tasks == 0; });
    const std::expected<GuileEvaluationResult, std::string> failed =
        guile.evaluate({.source = "failure-value", .source_name = "async-test.scm"});
    REQUIRE(failed.has_value());
    REQUIRE(failed->values.size() == 1);
    CHECK(failed->values.front().find("cannot start process") != std::string::npos);

    const std::expected<GuileEvaluationResult, std::string> shutdown_task =
        guile.evaluate({.source = R"((start-async-task!
 host
 (async-process "/bin/sh" '("-c" "sleep 30"))
 (lambda (task result) #f)))",
                        .source_name = "async-test.scm"});
    REQUIRE(shutdown_task.has_value());
    CHECK(guile.snapshot().outstanding_async_tasks == 1);
    guile.shutdown_async_tasks();
    CHECK(guile.snapshot().outstanding_async_tasks == 0);
    drain_until(async, [&] { return async_host.tasks().empty(); });
}

TEST_CASE("embedded Guile provides the vendored Scheme development runtime") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);

    const std::expected<GuileEvaluationResult, std::string> result =
        guile.evaluate({.source = R"((use-modules (ares version) (fibers config))
(list (version) ares-version %fibers-version))",
                        .source_name = "development-runtime-test.scm"});

    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->values == std::vector<std::string>{"(\"3.0.11\" \"0.9.7\" \"1.4.3\")"});
}

TEST_CASE("Ares endpoint commands own the nREPL server lifecycle") {
    EditorRuntime runtime;
    std::string message;
    GuileHostServices services;
    services.set_message = [&](std::string value) { message = std::move(value); };
    GuileRuntime guile(runtime, std::move(services));
    REQUIRE(guile.install_core_commands().has_value());
    const BufferId buffer = runtime.buffers().create({.name = "ares-test",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    const std::filesystem::path port_file =
        std::filesystem::temp_directory_path() / "cind-ares-endpoint-test.port";
    std::error_code error;
    std::filesystem::remove(port_file, error);

    const CommandResult started = runtime.commands().invoke(
        require_command(runtime, "scheme.ares-start"), context,
        CommandInvocation{.arguments = {port_file.string()}, .prefix = {}});
    REQUIRE(started.has_value());
    CHECK(std::holds_alternative<CommandCompleted>(*started));
    CHECK(message.find("Ares nREPL listening on 127.0.0.1:") != std::string::npos);
    REQUIRE(std::filesystem::exists(port_file));
    std::ifstream port_input(port_file);
    std::uint32_t port = 0;
    port_input >> port;
    CHECK(port >= 49152);
    CHECK(port <= 65535);

    const CommandResult status = runtime.commands().invoke(
        require_command(runtime, "scheme.ares-status"), context, CommandInvocation{});
    REQUIRE(status.has_value());
    CHECK(message.find(std::to_string(port)) != std::string::npos);

    const CommandResult stopped = runtime.commands().invoke(
        require_command(runtime, "scheme.ares-stop"), context, CommandInvocation{});
    REQUIRE(stopped.has_value());
    CHECK(message == "Ares nREPL is stopped");
    CHECK_FALSE(std::filesystem::exists(port_file));
}

TEST_CASE("bundled Guile policy installs available default key bindings") {
    EditorRuntime runtime;
    const CommandId save = define_command(runtime, "file.save");
    const CommandId replace = define_command(runtime, "search.replace");
    const CommandId quit = define_command(runtime, "application.quit");
    const CommandId keyboard_quit = define_command(runtime, "keyboard.quit");
    const CommandId interaction_submit = define_command(runtime, "interaction.submit");
    const CommandId interaction_next = define_command(runtime, "interaction.next-candidate");
    const CommandId interaction_previous =
        define_command(runtime, "interaction.previous-candidate");
    const CommandId history_previous = define_command(runtime, "interaction.previous-history");
    const CommandId history_next = define_command(runtime, "interaction.next-history");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> installed = guile.install_default_keymaps();

    REQUIRE(installed.has_value());
    CHECK(*installed == 15);
    const KeymapId editor = require_keymap(runtime, "editor.default");
    const KeymapId application = require_keymap(runtime, "application.global");
    const KeymapId control_x = require_keymap(runtime, "editor.control-x");
    const KeymapId system = require_keymap(runtime, "editor.system");
    const KeymapId interaction_text = require_keymap(runtime, "interaction.text");
    const KeymapId interaction_picker = require_keymap(runtime, "interaction.picker");
    CHECK(resolve_command(runtime, editor, "C-x C-s") == save);
    CHECK(resolve_command(runtime, editor, "M-%") == replace);
    CHECK(resolve_command(runtime, application, "C-x C-c") == quit);
    CHECK(resolve_command(runtime, system, "C-g") == keyboard_quit);
    CHECK(resolve_command(runtime, interaction_text, "RET") == interaction_submit);
    CHECK(resolve_command(runtime, interaction_text, "ESC") == keyboard_quit);
    CHECK(resolve_command(runtime, interaction_text, "M-p") == history_previous);
    CHECK(resolve_command(runtime, interaction_text, "M-n") == history_next);
    CHECK(resolve_command(runtime, interaction_picker, "C-n") == interaction_next);
    CHECK(resolve_command(runtime, interaction_picker, "C-p") == interaction_previous);
    CHECK(runtime.keymaps().parent(interaction_text) ==
          runtime.keymaps().find("editor.text-input"));
    CHECK(runtime.keymaps().parent(interaction_picker) == interaction_text);
    const std::vector<KeymapCompletion> root = runtime.keymaps().completions(editor, {});
    const auto prefix = std::ranges::find_if(root, [](const KeymapCompletion& completion) {
        return format_key_stroke(completion.key) == "C-x";
    });
    REQUIRE(prefix != root.end());
    CHECK(prefix->prefix_keymap == control_x);
    CHECK(prefix->label == "C-x");

    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.engine == "guile");
    CHECK(snapshot.version == "3.0.11");
    CHECK(snapshot.modules == std::vector<std::string>{
                                  "cind command", "cind input", "cind async", "cind extension",
                                  "cind emacs", "cind toy-modal", "cind meow", "cind vim",
                                  "cind helix", "cind structural", "cind minibuffer",
                                  "cind development", "cind ares", "cind introspect", "cind core"});
    CHECK(snapshot.binding_revision == 1);
    CHECK_FALSE(snapshot.last_error.has_value());
}

TEST_CASE("Guile keymap policy treats unavailable commands as optional") {
    EditorRuntime runtime;
    const CommandId save = define_command(runtime, "file.save");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> first = guile.install_default_keymaps();
    const std::expected<std::size_t, std::string> second = guile.install_default_keymaps();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 2);
    CHECK(*second == 2);
    const KeymapId editor = require_keymap(runtime, "editor.default");
    CHECK(resolve_command(runtime, editor, "C-x C-s") == save);
    CHECK(guile.snapshot().binding_revision == 2);
}

TEST_CASE("bundled Guile policy defines the default input state") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);

    const std::expected<std::size_t, std::string> first = guile.install_input_states();
    const std::expected<std::size_t, std::string> second = guile.install_input_states();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 17);
    CHECK(*second == 17);
    const InputStateId emacs = runtime.input_states().find("emacs").value_or(InputStateId{});
    REQUIRE(emacs);
    const InputStateRegistry::Definition& definition = runtime.input_states().definition(emacs);
    CHECK(definition.keymaps.empty());
    CHECK(definition.text_input == TextInputPolicy::Accept);
    CHECK(definition.text_command == std::optional<std::string>{"edit.self-insert"});
    CHECK(definition.cursor == CursorShape::Beam);
    CHECK_FALSE(definition.handler);
    const InputStateId universal =
        runtime.input_states().find("emacs-universal").value_or(InputStateId{});
    REQUIRE(universal);
    CHECK(runtime.input_states().definition(universal).handler);
    CHECK(runtime.input_states().definition(universal).on_enter);
    CHECK(runtime.input_states().definition(universal).on_exit);
    CHECK(runtime.input_states().definition(universal).indicator == "ARG");
    CHECK(guile.snapshot().input_state_revision == 2);
    const InputStateId toy = runtime.input_states().find("toy-normal").value_or(InputStateId{});
    REQUIRE(toy);
    const InputStateRegistry::Definition& toy_definition = runtime.input_states().definition(toy);
    CHECK(toy_definition.text_input == TextInputPolicy::Ignore);
    CHECK(toy_definition.cursor == CursorShape::Block);
    CHECK(toy_definition.indicator == "N");
    REQUIRE(toy_definition.keymaps.size() == 1);
    CHECK(runtime.keymaps().definition(toy_definition.keymaps.front()).name == "toy-modal.normal");
    const InputStateId keypad = runtime.input_states().find("meow-keypad").value_or(InputStateId{});
    REQUIRE(keypad);
    CHECK(runtime.input_states().definition(keypad).handler);
    CHECK(runtime.input_states().definition(keypad).on_exit);
    CHECK_FALSE(runtime.input_states().definition(keypad).on_enter);
    const InputStateId numeric =
        runtime.input_states().find("meow-numeric").value_or(InputStateId{});
    REQUIRE(numeric);
    CHECK(runtime.input_states().definition(numeric).handler);
    CHECK(runtime.input_states().definition(numeric).on_exit);
    const InputStateId read_key =
        runtime.input_states().find("input.read-key").value_or(InputStateId{});
    REQUIRE(read_key);
    CHECK(runtime.input_states().definition(read_key).handler);
    CHECK(runtime.input_states().definition(read_key).on_exit);
    CHECK(runtime.input_states().definition(read_key).indicator == "KEY");
    const InputStrategyId meow =
        runtime.input_strategies().find("meow").value_or(InputStrategyId{});
    REQUIRE(meow);
    CHECK(runtime.input_states()
              .definition(runtime.input_strategies().state(meow, InteractionClass::Editing))
              .name == "meow-normal");
    CHECK(runtime.input_states()
              .definition(runtime.input_strategies().state(meow, InteractionClass::Interface))
              .name == "meow-motion");
    const InputStateId meow_normal =
        runtime.input_states().find("meow-normal").value_or(InputStateId{});
    REQUIRE(meow_normal);
    CHECK(runtime.input_states().definition(meow_normal).position_hints);
    CHECK_FALSE(runtime.input_states().definition(keypad).position_hints);
    CHECK(guile.snapshot().scripted_input_states == 17);
    CHECK(guile.snapshot().scripted_input_strategies == 5);
    const InputStrategyId helix =
        runtime.input_strategies().find("helix").value_or(InputStrategyId{});
    const InputStrategyId vim = runtime.input_strategies().find("vim").value_or(InputStrategyId{});
    const InputStateId structural =
        runtime.input_states().find("structural-node").value_or(InputStateId{});
    const InputStateId vim_operator =
        runtime.input_states().find("vim-operator").value_or(InputStateId{});
    const InputStrategyId emacs_strategy =
        runtime.input_strategies().find("emacs").value_or(InputStrategyId{});
    const InputStrategyId toy_strategy =
        runtime.input_strategies().find("toy-modal").value_or(InputStrategyId{});
    REQUIRE(emacs_strategy);
    REQUIRE(toy_strategy);
    REQUIRE(vim);
    REQUIRE(helix);
    REQUIRE(structural);
    REQUIRE(vim_operator);
    CHECK(runtime.input_states().definition(structural).indicator == "NODE");
    CHECK(runtime.input_states().definition(structural).text_input == TextInputPolicy::Ignore);
    CHECK(runtime.input_states().definition(structural).on_exit);
    CHECK(runtime.input_states().definition(vim_operator).on_exit);
    CHECK(runtime.input_strategies().default_strategy() == emacs_strategy);
    CHECK(runtime.input_strategies().definition(emacs_strategy).selection_after_edit ==
          SelectionEditPolicy::Collapse);
    CHECK(runtime.input_strategies().definition(meow).selection_after_edit ==
          SelectionEditPolicy::Collapse);
    CHECK(runtime.input_strategies().state(emacs_strategy, InteractionClass::Editing) == emacs);
    CHECK(runtime.input_strategies().state(emacs_strategy, InteractionClass::Interface) == emacs);
    CHECK(runtime.input_strategies().state(toy_strategy, InteractionClass::Editing) == toy);
    CHECK(runtime.input_strategies().state(toy_strategy, InteractionClass::Interface) == emacs);
    CHECK(runtime.input_states()
              .definition(runtime.input_strategies().state(vim, InteractionClass::Editing))
              .name == "vim-normal");
    CHECK(runtime.input_states()
              .definition(runtime.input_strategies().state(helix, InteractionClass::Editing))
              .name == "hx-normal");
    CHECK(runtime.input_strategies().definition(helix).selection_after_edit ==
          SelectionEditPolicy::Preserve);
}

TEST_CASE("bundled Guile policy declares the core mode hierarchy") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);
    REQUIRE(guile.install_input_states().has_value());

    const std::expected<std::size_t, std::string> first = guile.install_core_modes();
    const std::expected<std::size_t, std::string> second = guile.install_core_modes();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 4);
    CHECK(*second == 4);
    const ModeId fundamental = runtime.modes().find("fundamental-mode").value_or(ModeId{});
    const ModeId prog = runtime.modes().find("prog-mode").value_or(ModeId{});
    const ModeId special = runtime.modes().find("special-mode").value_or(ModeId{});
    const ModeId scheme = runtime.modes().find("scheme-mode").value_or(ModeId{});
    REQUIRE(fundamental);
    REQUIRE(prog);
    REQUIRE(special);
    REQUIRE(scheme);
    CHECK(runtime.modes().definition(prog).parent == fundamental);
    CHECK(runtime.modes().definition(prog).things ==
          std::vector<ModeThingBinding>{{.name = "word", .definition = "cind.word"},
                                        {.name = "symbol", .definition = "cind.symbol"}});
    CHECK(runtime.things().find("cind.angle").has_value());
    CHECK(runtime.things().find("cind.defun").has_value());
    CHECK(runtime.motions().find("cind.forward-word").has_value());
    CHECK(runtime.motions().find("cind.forward-symbol").has_value());
    CHECK(runtime.modes().definition(special).parent == fundamental);
    CHECK(runtime.modes().definition(special).interaction_class == InteractionClass::Interface);
    CHECK(runtime.modes().definition(scheme).parent == prog);
    CHECK(runtime.modes().definition(scheme).keymaps ==
          std::vector<KeymapId>{require_keymap(runtime, "scheme-mode-map")});
    const InputStrategyId emacs_strategy =
        runtime.input_strategies().find("emacs").value_or(InputStrategyId{});
    REQUIRE(emacs_strategy);
    CHECK(runtime.input_strategies().default_strategy() == emacs_strategy);
    CHECK(guile.snapshot().mode_revision == 2);
    CHECK(guile.snapshot().scripted_modes == 4);
}

TEST_CASE("bundled Guile policy defines file modes and project discovery providers") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);
    REQUIRE(guile.install_input_states().has_value());
    REQUIRE(guile.install_core_modes().has_value());
    const ModeId cpp = ensure_cpp_mode(runtime).mode;

    const std::expected<std::size_t, std::string> first = guile.install_core_resource_policies();
    const std::expected<std::size_t, std::string> second = guile.install_core_resource_policies();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 5);
    CHECK(*second == 5);
    CHECK(runtime.resource_policies().mode_for("src/main.cpp") == cpp);
    CHECK(runtime.resource_policies().mode_for("module.scm") ==
          runtime.modes().find("scheme-mode"));
    CHECK_FALSE(runtime.resource_policies().mode_for("README.md").has_value());
    CHECK(runtime.resource_policies().file_mode_rules().size() == 2);
    CHECK(runtime.resource_policies().project_providers().size() == 3);
    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.resource_policy_revision == 2);
    CHECK(snapshot.scripted_file_mode_rules == 2);
    CHECK(snapshot.scripted_project_providers == 3);
}

TEST_CASE("bundled Guile commands return editor command actions") {
    EditorRuntime runtime;

    const BufferId buffer = runtime.buffers().create({.name = "sample",
                                                      .initial_text = "abc\n",
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
    WindowId displayed_help_window;
    std::string help_name;
    std::string help_text;
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
    BufferId saved_buffer;
    bool buffer_saved = false;
    BufferId killed_buffer;
    bool kill_forced = false;
    bool buffer_killed = false;
    std::string kill_error;
    bool quit_requested = false;
    bool quit_forced = false;
    WindowId split_target;
    WindowSplitAxis split_axis = WindowSplitAxis::Rows;
    bool window_split = false;
    WindowId deleted_window;
    bool window_deleted = false;
    WindowId retained_window;
    bool other_windows_deleted = false;
    WindowId other_window_source;
    int other_window_delta = 0;
    bool other_window_selected = false;
    bool redraw_requested = false;
    std::string window_error;
    std::string clipboard;
    std::string clipboard_error;
    std::optional<GuileTextRange> requested_soft_kill_range;
    std::optional<std::uint32_t> requested_motion_target;
    std::optional<ViewId> requested_motion_view;
    std::optional<ViewSelection> requested_motion_source;
    std::optional<std::string> requested_motion;
    std::int64_t requested_motion_count = 0;
    bool requested_motion_extend = false;
    GuileRuntime guile(
        runtime,
        {.display_buffer = [&](WindowId target_window,
                               BufferId target_buffer) -> std::expected<void, std::string> {
             displayed = std::pair{target_window, target_buffer};
             buffer_displayed = true;
             return {};
         },
         .display_generated_buffer = [&](WindowId target_window, std::string name,
                                         std::string text) -> std::expected<void, std::string> {
             displayed_help_window = target_window;
             help_name = std::move(name);
             help_text = std::move(text);
             return {};
         },
         .move_caret_to_line = [&](ViewId target_view, std::uint32_t line,
                                   std::uint32_t column) -> std::expected<void, std::string> {
             moved = std::tuple{target_view, line, column};
             caret_moved = true;
             return {};
         },
         .undo = {},
         .redo = {},
         .set_view_caret = {},
         .move_caret_lines = {},
         .move_caret_line_boundary = {},
         .delete_grapheme = {},
         .newline = {},
         .indent = {},
         .type_text = {},
         .page_rows = {},
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
         },
         .save_buffer =
             [&](BufferId target_buffer) {
                 saved_buffer = target_buffer;
                 buffer_saved = true;
             },
         .open_buffers = [&] { return std::vector<BufferId>{buffer, other}; },
         .kill_buffer = [&](BufferId target_buffer,
                            bool force) -> std::expected<void, std::string> {
             if (!kill_error.empty()) {
                 return std::unexpected(kill_error);
             }
             killed_buffer = target_buffer;
             kill_forced = force;
             buffer_killed = true;
             return {};
         },
         .request_quit =
             [&](bool force) {
                 quit_requested = true;
                 quit_forced = force;
             },
         .split_window = [&](WindowId target,
                             WindowSplitAxis axis) -> std::expected<void, std::string> {
             if (!window_error.empty()) {
                 return std::unexpected(window_error);
             }
             split_target = target;
             split_axis = axis;
             window_split = true;
             return {};
         },
         .delete_window = [&](WindowId target) -> std::expected<void, std::string> {
             if (!window_error.empty()) {
                 return std::unexpected(window_error);
             }
             deleted_window = target;
             window_deleted = true;
             return {};
         },
         .delete_other_windows =
             [&](WindowId retained) {
                 retained_window = retained;
                 other_windows_deleted = true;
             },
         .select_other_window = [&](WindowId source,
                                    int delta) -> std::expected<void, std::string> {
             if (!window_error.empty()) {
                 return std::unexpected(window_error);
             }
             other_window_source = source;
             other_window_delta = delta;
             other_window_selected = true;
             return {};
         },
         .request_redraw = [&] { redraw_requested = true; },
         .active_key_bindings =
             [] {
                 return std::vector<GuileKeyBindingSummary>{
                     {.keys = "C-x C-s", .command = "file.save"}};
             },
         .active_keymap_layers = [](WindowId) { return std::vector<KeymapId>{}; },
         .base_keymap_layers = [](WindowId) { return std::vector<KeymapId>{}; },
         .set_selection =
             [&](ViewId target, ViewSelection selection) {
                 runtime.views().set_selection(target, std::move(selection));
             },
         .clear_selection = [&](ViewId target) { runtime.views().clear_selection(target); },
         .replace_selection = [&](ViewId target, ViewSelection selection,
                                  std::vector<std::string> replacements)
             -> std::expected<ViewSelection, std::string> {
             Buffer& target_buffer = runtime.buffers().get(runtime.views().get(target).buffer_id());
             struct Edit {
                 std::size_t index;
                 TextRange range;
                 std::string text;
             };
             std::vector<Edit> edits;
             edits.reserve(selection.ranges.size());
             for (std::size_t index = 0; index < selection.ranges.size(); ++index) {
                 edits.push_back({index, selection.ranges[index].ordered(), replacements[index]});
             }
             std::ranges::sort(edits, [](const Edit& left, const Edit& right) {
                 return left.range.start < right.range.start;
             });
             EditTransaction transaction = target_buffer.begin_transaction();
             for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
                 transaction.replace(edit->range, edit->text);
             }
             (void)transaction.commit();
             return selection;
         },
         .selection_texts = [&](ViewId target, const ViewSelection& selection)
             -> std::expected<std::vector<std::string>, std::string> {
             const DocumentSnapshot snapshot =
                 runtime.buffers().get(runtime.views().get(target).buffer_id()).snapshot();
             std::vector<std::string> texts;
             texts.reserve(selection.ranges.size());
             for (const SelectionRange& range : selection.ranges) {
                 texts.push_back(snapshot.substring(range.ordered()));
             }
             return texts;
         },
         .erase_range = [&](ViewId target,
                            GuileTextRange range) -> std::expected<void, std::string> {
             Buffer& target_buffer = runtime.buffers().get(runtime.views().get(target).buffer_id());
             EditTransaction transaction = target_buffer.begin_transaction();
             transaction.erase(TextRange{TextOffset{range.start}, TextOffset{range.end}});
             (void)transaction.commit();
             runtime.views().set_caret(target, TextOffset{range.start});
             return {};
         },
         .insert_text = [&](ViewId target,
                            std::vector<std::string> texts) -> std::expected<void, std::string> {
             Buffer& target_buffer = runtime.buffers().get(runtime.views().get(target).buffer_id());
             const TextOffset caret = runtime.views().caret(target);
             EditTransaction transaction = target_buffer.begin_transaction();
             transaction.insert(caret, texts.front());
             (void)transaction.commit();
             runtime.views().set_caret(target, TextOffset{caret.value + static_cast<std::uint32_t>(
                                                                            texts.front().size())});
             return {};
         },
         .soft_kill_range = [&](ViewId) { return requested_soft_kill_range; },
         .thing_selection = [](ViewId, const ViewSelection&, std::string_view,
                               bool) -> std::expected<std::optional<ViewSelection>, std::string> {
             return std::optional<ViewSelection>{};
         },
         .motion_selection = [&](ViewId target, const ViewSelection& source,
                                 std::string_view motion, std::int64_t count,
                                 bool extend) -> std::expected<ViewSelection, std::string> {
             requested_motion_view = target;
             requested_motion_source = source;
             requested_motion = motion;
             requested_motion_count = count;
             requested_motion_extend = extend;
             const TextOffset destination{requested_motion_target.value_or(0)};
             ViewSelection result = source;
             result.ranges[result.primary].head = destination;
             if (!extend) {
                 result.ranges[result.primary].anchor = destination;
             }
             return result;
         },
         .expand_selection = [](ViewId, const ViewSelection&)
             -> std::expected<std::optional<ViewSelection>, std::string> {
             return std::optional<ViewSelection>{};
         },
         .write_clipboard = [&](std::string_view text) -> std::expected<void, std::string> {
             if (!clipboard_error.empty()) {
                 return std::unexpected(clipboard_error);
             }
             clipboard = text;
             return {};
         },
         .read_clipboard = [&]() -> std::expected<std::optional<std::string>, std::string> {
             if (!clipboard_error.empty()) {
                 return std::unexpected(clipboard_error);
             }
             return clipboard.empty() ? std::optional<std::string>{}
                                      : std::optional<std::string>{clipboard};
         },
         .start_async_task = {},
         .cancel_async_task = {},
         .async_tasks = {}});
    const std::expected<std::size_t, std::string> installed = guile.install_core_commands();
    REQUIRE(installed.has_value());
    CHECK(*installed == 178);
    const std::expected<std::size_t, std::string> providers = guile.install_core_providers();
    REQUIRE(providers.has_value());
    CHECK(*providers == 6);
    const CommandId save = require_command(runtime, "file.save");

    const CommandResult saved = runtime.commands().invoke(save, context);
    REQUIRE(saved.has_value());
    REQUIRE(buffer_saved);
    CHECK(saved_buffer == buffer);

    const std::vector<InteractionCandidate> command_candidates =
        complete_provider(runtime, "commands", context);
    CHECK(std::ranges::any_of(command_candidates, [](const InteractionCandidate& candidate) {
        return candidate.value == "file.save" && candidate.detail == "command";
    }));
    CHECK(std::ranges::none_of(command_candidates, [](const InteractionCandidate& candidate) {
        return candidate.value.ends_with(".accept") || candidate.value.starts_with("interaction.");
    }));

    const std::vector<InteractionCandidate> buffer_candidates =
        complete_provider(runtime, "buffers", context);
    REQUIRE(buffer_candidates.size() == 2);
    CHECK(buffer_candidates[0].value == "sample");
    CHECK(buffer_candidates[1].value == "other");

    const std::vector<InteractionCandidate> binding_candidates =
        complete_provider(runtime, "key-bindings", context);
    REQUIRE(binding_candidates.size() == 1);
    CHECK(binding_candidates[0].value == "C-x C-s  file.save");
    CHECK(binding_candidates[0].label == "C-x C-s");
    CHECK(binding_candidates[0].detail == "file.save");

    const std::vector<InteractionCandidate> function_candidates =
        complete_provider(runtime, "scheme-functions", context);
    const auto describe_mode_function =
        std::ranges::find_if(function_candidates, [](const InteractionCandidate& candidate) {
            return candidate.label == "describe-mode" && candidate.detail == "(cind introspect)";
        });
    REQUIRE(describe_mode_function != function_candidates.end());

    const CommandResult function_request_result =
        runtime.commands().invoke(require_command(runtime, "help.describe-function"), context);
    REQUIRE(function_request_result.has_value());
    const auto* function_request = std::get_if<InteractionRequest>(&*function_request_result);
    REQUIRE(function_request != nullptr);
    CHECK(function_request->provider == "scheme-functions");
    const CommandResult function_help = runtime.commands().invoke(
        function_request->accept_command, context,
        CommandInvocation{.arguments = {describe_mode_function->value}, .prefix = {}});
    REQUIRE(function_help.has_value());
    CHECK(displayed_help_window == window);
    CHECK(help_text.find("describe-mode is defined in (cind introspect)") != std::string::npos);
    CHECK(help_text.find("Kind: function") != std::string::npos);

    const CommandResult mode_help =
        runtime.commands().invoke(require_command(runtime, "help.describe-mode"), context);
    REQUIRE(mode_help.has_value());
    CHECK(help_text.find("Mode state for the current buffer") != std::string::npos);
    CHECK(help_text.find("Effective policy") != std::string::npos);

    const std::vector<InteractionCandidate> variable_candidates =
        complete_provider(runtime, "scheme-variables", context);
    const auto help_name_variable =
        std::ranges::find_if(variable_candidates, [](const InteractionCandidate& candidate) {
            return candidate.label == "help-buffer-name" && candidate.detail == "(cind introspect)";
        });
    REQUIRE(help_name_variable != variable_candidates.end());
    const CommandResult variable_request_result =
        runtime.commands().invoke(require_command(runtime, "help.describe-variable"), context);
    REQUIRE(variable_request_result.has_value());
    const auto* variable_request = std::get_if<InteractionRequest>(&*variable_request_result);
    REQUIRE(variable_request != nullptr);
    const CommandResult variable_help = runtime.commands().invoke(
        variable_request->accept_command, context,
        CommandInvocation{.arguments = {help_name_variable->value}, .prefix = {}});
    REQUIRE(variable_help.has_value());
    CHECK(help_text.find("help-buffer-name is defined in (cind introspect)") != std::string::npos);
    CHECK(help_text.find("\"*Help*\"") != std::string::npos);

    const CommandId describe_bindings = require_command(runtime, "help.describe-bindings");
    const CommandResult bindings_help = runtime.commands().invoke(describe_bindings, context);
    REQUIRE(bindings_help.has_value());
    CHECK(help_name == "*Help*");
    CHECK(help_text.find("Active key bindings") != std::string::npos);
    CHECK(help_text.find("C-x C-s") != std::string::npos);

    const CommandId describe_command = require_command(runtime, "help.describe-command");
    const CommandResult describe_request_result =
        runtime.commands().invoke(describe_command, context);
    REQUIRE(describe_request_result.has_value());
    const auto* describe_request = std::get_if<InteractionRequest>(&*describe_request_result);
    REQUIRE(describe_request != nullptr);
    CHECK(describe_request->provider == "commands");
    const CommandResult described = runtime.commands().invoke(
        describe_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("help.describe-command")}, .prefix = {}});
    REQUIRE(described.has_value());
    CHECK(help_text.find("help.describe-command is a command") != std::string::npos);
    CHECK(help_text.find("Display the registry metadata") != std::string::npos);
    CHECK(runtime.commands().definition(describe_command).source == "scheme:(cind core)");

    const CommandId palette = require_command(runtime, "command.palette");
    const CommandResult palette_result = runtime.commands().invoke(palette, context);
    REQUIRE(palette_result.has_value());
    const auto* request = std::get_if<InteractionRequest>(&*palette_result);
    REQUIRE(request != nullptr);
    CHECK(request->kind == InteractionKind::Picker);
    CHECK(request->keymap == "interaction.picker");
    CHECK(request->input_state == "emacs");
    CHECK(request->prompt == "Command: ");
    CHECK(request->provider == "commands");
    CHECK(runtime.commands().definition(request->accept_command).name == "command.palette.accept");

    const CommandResult search_prompt_result =
        runtime.commands().invoke(require_command(runtime, "search.prompt"), context);
    REQUIRE(search_prompt_result.has_value());
    const auto* search_prompt = std::get_if<InteractionRequest>(&*search_prompt_result);
    REQUIRE(search_prompt != nullptr);
    CHECK(search_prompt->kind == InteractionKind::Text);
    CHECK(search_prompt->keymap == "interaction.text");
    CHECK(search_prompt->input_state == "emacs");
    CHECK(search_prompt->prompt == "search: ");
    CHECK(search_prompt->history == "search");
    CHECK(search_prompt->accept_command == require_command(runtime, "search.accept"));
    REQUIRE(search_prompt->arguments.size() == 1);
    CHECK(std::get<bool>(search_prompt->arguments.front()));

    const CommandResult backward_search_prompt_result =
        runtime.commands().invoke(require_command(runtime, "search.backward-prompt"), context);
    REQUIRE(backward_search_prompt_result.has_value());
    const auto* backward_search_prompt =
        std::get_if<InteractionRequest>(&*backward_search_prompt_result);
    REQUIRE(backward_search_prompt != nullptr);
    CHECK(backward_search_prompt->prompt == "search backward: ");
    REQUIRE(backward_search_prompt->arguments.size() == 1);
    CHECK_FALSE(std::get<bool>(backward_search_prompt->arguments.front()));

    const CommandId search_accept = require_command(runtime, "search.accept");
    const CommandResult buffer_search_result = runtime.commands().invoke(
        search_accept, context,
        CommandInvocation{.arguments = {true, std::string("bc")}, .prefix = {}});
    REQUIRE(buffer_search_result.has_value());
    CHECK(runtime.views().caret(view) == TextOffset{1});
    CHECK(message.empty());
    CHECK(redraw_requested);
    CHECK(runtime.commands().definition(search_accept).source == "scheme:(cind core)");

    message.clear();
    const CommandId search_next = require_command(runtime, "search.next");
    REQUIRE(runtime.commands().invoke(search_next, context).has_value());
    CHECK(runtime.views().caret(view) == TextOffset{1});
    CHECK(message == "search wrapped");
    CHECK(runtime.commands().definition(search_next).source == "scheme:(cind core)");

    message.clear();
    const CommandId search_previous = require_command(runtime, "search.previous");
    REQUIRE(runtime.commands().invoke(search_previous, context).has_value());
    CHECK(runtime.views().caret(view) == TextOffset{1});
    CHECK(message == "search wrapped");
    CHECK(runtime.commands().definition(search_previous).source == "scheme:(cind core)");

    const CommandResult missing = runtime.commands().invoke(request->accept_command, context);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message == "command palette requires a command name");

    const CommandResult accepted = runtime.commands().invoke(
        request->accept_command, context,
        CommandInvocation{.arguments = {std::string("file.save")}, .prefix = {}});
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
    const CommandResult directory_result =
        runtime.commands().invoke(open_request->accept_command, context,
                                  CommandInvocation{.arguments = {directory}, .prefix = {}});
    REQUIRE(directory_result.has_value());
    const auto* directory_request = std::get_if<InteractionRequest>(&*directory_result);
    REQUIRE(directory_request != nullptr);
    CHECK(directory_request->initial_input == directory);
    CHECK_FALSE(file_opened);

    const CommandResult opened = runtime.commands().invoke(
        open_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("/tmp/example.cpp")}, .prefix = {}});
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
    const CommandResult save_as_accepted = runtime.commands().invoke(
        save_as_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("/tmp/written.cpp")}, .prefix = {}});
    REQUIRE(save_as_accepted.has_value());
    REQUIRE(buffer_resource_set);
    CHECK(resource_buffer == buffer);
    CHECK(resource_path == "/tmp/written.cpp");
    const auto* save_dispatch = std::get_if<CommandDispatch>(&*save_as_accepted);
    REQUIRE(save_dispatch != nullptr);
    CHECK(save_dispatch->command == save);

    const CommandResult switched = runtime.commands().invoke(
        require_command(runtime, "buffer.switch.accept"), context,
        CommandInvocation{.arguments = {std::string("other")}, .prefix = {}});
    REQUIRE(switched.has_value());
    REQUIRE(buffer_displayed);
    CHECK(displayed.first == window);
    CHECK(displayed.second == other);

    buffer_displayed = false;
    const CommandResult next_buffer =
        runtime.commands().invoke(require_command(runtime, "buffer.next"), context);
    REQUIRE(next_buffer.has_value());
    REQUIRE(buffer_displayed);
    CHECK(displayed.first == window);
    CHECK(displayed.second == other);

    const CommandResult killed =
        runtime.commands().invoke(require_command(runtime, "buffer.kill"), context);
    REQUIRE(killed.has_value());
    REQUIRE(buffer_killed);
    CHECK(killed_buffer == buffer);
    CHECK_FALSE(kill_forced);

    buffer_killed = false;
    const CommandResult force_killed =
        runtime.commands().invoke(require_command(runtime, "buffer.force-kill"), context);
    REQUIRE(force_killed.has_value());
    REQUIRE(buffer_killed);
    CHECK(killed_buffer == buffer);
    CHECK(kill_forced);

    kill_error = "buffer has unsaved changes";
    const CommandResult refused_kill =
        runtime.commands().invoke(require_command(runtime, "buffer.kill"), context);
    REQUIRE_FALSE(refused_kill.has_value());
    CHECK(refused_kill.error().message == "buffer has unsaved changes");
    kill_error.clear();

    const CommandResult quit =
        runtime.commands().invoke(require_command(runtime, "application.quit"), context);
    REQUIRE(quit.has_value());
    REQUIRE(quit_requested);
    CHECK_FALSE(quit_forced);
    quit_requested = false;
    const CommandResult force_quit =
        runtime.commands().invoke(require_command(runtime, "application.force-quit"), context);
    REQUIRE(force_quit.has_value());
    REQUIRE(quit_requested);
    CHECK(quit_forced);

    const CommandResult split_below =
        runtime.commands().invoke(require_command(runtime, "window.split-below"), context);
    REQUIRE(split_below.has_value());
    REQUIRE(window_split);
    CHECK(split_target == window);
    CHECK(split_axis == WindowSplitAxis::Rows);
    window_split = false;
    const CommandResult split_right =
        runtime.commands().invoke(require_command(runtime, "window.split-right"), context);
    REQUIRE(split_right.has_value());
    REQUIRE(window_split);
    CHECK(split_axis == WindowSplitAxis::Columns);

    const CommandResult delete_others =
        runtime.commands().invoke(require_command(runtime, "window.delete-others"), context);
    REQUIRE(delete_others.has_value());
    REQUIRE(other_windows_deleted);
    CHECK(retained_window == window);

    const CommandResult other_window =
        runtime.commands().invoke(require_command(runtime, "window.other"), context);
    REQUIRE(other_window.has_value());
    REQUIRE(other_window_selected);
    CHECK(other_window_source == window);
    CHECK(other_window_delta == 1);

    const CommandResult redraw =
        runtime.commands().invoke(require_command(runtime, "editor.redraw"), context);
    REQUIRE(redraw.has_value());
    CHECK(redraw_requested);

    redraw_requested = false;
    const ViewSelection motion_source{.ranges = {{.anchor = TextOffset{0},
                                                  .head = TextOffset{0},
                                                  .granularity = SelectionGranularity::Character}},
                                      .primary = 0,
                                      .metadata = "((source . explicit))"};
    runtime.views().set_selection(view, motion_source);
    runtime.views().get(view).viewport().preferred_column = 9;
    requested_motion_target = 2;
    CommandLoop motion_loop(runtime);
    CHECK(motion_loop.execute(require_command(runtime, "cursor.forward-expression"), context)
              .status == CommandLoopStatus::Executed);
    CHECK(requested_motion_view == std::optional<ViewId>{view});
    CHECK(requested_motion_source == std::optional<ViewSelection>{motion_source});
    CHECK(requested_motion == std::optional<std::string>{"cind.forward-expression"});
    CHECK(requested_motion_count == 1);
    CHECK_FALSE(requested_motion_extend);
    CHECK(runtime.views().caret(view) == TextOffset{2});
    CHECK_FALSE(runtime.views().get(view).viewport().preferred_column.has_value());
    CHECK(redraw_requested);

    CHECK(motion_loop.execute(require_command(runtime, "cursor.backward-expression"), context)
              .status == CommandLoopStatus::Executed);
    CHECK(requested_motion == std::optional<std::string>{"cind.backward-expression"});
    CHECK(motion_loop.execute(require_command(runtime, "cursor.up-list"), context).status ==
          CommandLoopStatus::Executed);
    CHECK(requested_motion == std::optional<std::string>{"cind.up-list"});

    window_error = "cannot delete the only window";
    const CommandResult refused_delete =
        runtime.commands().invoke(require_command(runtime, "window.delete"), context);
    REQUIRE_FALSE(refused_delete.has_value());
    CHECK(refused_delete.error().message == "cannot delete the only window");
    CHECK_FALSE(window_deleted);
    window_error.clear();
    const CommandResult deleted =
        runtime.commands().invoke(require_command(runtime, "window.delete"), context);
    REQUIRE(deleted.has_value());
    REQUIRE(window_deleted);
    CHECK(deleted_window == window);

    const CommandResult unknown_buffer = runtime.commands().invoke(
        require_command(runtime, "buffer.switch.accept"), context,
        CommandInvocation{.arguments = {std::string("missing")}, .prefix = {}});
    REQUIRE_FALSE(unknown_buffer.has_value());
    CHECK(unknown_buffer.error().message == "unknown buffer 'missing'");

    const CommandResult moved_to_line = runtime.commands().invoke(
        require_command(runtime, "cursor.goto-line.accept"), context,
        CommandInvocation{.arguments = {std::string("4:7")}, .prefix = {}});
    REQUIRE(moved_to_line.has_value());
    REQUIRE(caret_moved);
    CHECK(std::get<0>(moved) == view);
    CHECK(std::get<1>(moved) == 3);
    CHECK(std::get<2>(moved) == 6);

    const CommandResult invalid_line =
        runtime.commands().invoke(require_command(runtime, "cursor.goto-line.accept"), context,
                                  CommandInvocation{.arguments = {std::string("0")}, .prefix = {}});
    REQUIRE_FALSE(invalid_line.has_value());
    CHECK(invalid_line.error().message == "invalid line number");

    const CommandResult help = runtime.commands().invoke(
        require_command(runtime, "help.keys.accept"), context,
        CommandInvocation{.arguments = {std::string("C-x C-s  file.save")}, .prefix = {}});
    REQUIRE(help.has_value());
    CHECK(message == "C-x C-s  file.save");

    const CommandId project_search = require_command(runtime, "project.search");
    CHECK_FALSE(runtime.commands().enabled(project_search, context));
    const ProjectId project = runtime.projects().create({.name = "sample",
                                                         .roots = {"/tmp/sample"},
                                                         .discovery_provider = {},
                                                         .discovery_marker = {}});
    runtime.projects().assign(buffer, project);
    runtime.projects().replace_index(project,
                                     {"/tmp/sample/src/main.cpp", "/tmp/sample/include/main.hpp"});
    CHECK(runtime.commands().enabled(project_search, context));

    const std::vector<InteractionCandidate> project_candidates =
        complete_provider(runtime, "project-files", context);
    REQUIRE(project_candidates.size() == 2);
    CHECK(project_candidates[0].value == "/tmp/sample/include/main.hpp");
    CHECK(project_candidates[0].label == "include/main.hpp");
    CHECK(project_candidates[0].detail == "include");

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
        CommandInvocation{.arguments = {std::string("/tmp/sample/main.cpp")}, .prefix = {}});
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
    const CommandResult empty_search =
        runtime.commands().invoke(search_request->accept_command, context,
                                  CommandInvocation{.arguments = {std::string()}, .prefix = {}});
    REQUIRE_FALSE(empty_search.has_value());
    CHECK(empty_search.error().message == "project search query is empty");
    const CommandResult search_accepted = runtime.commands().invoke(
        search_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("needle")}, .prefix = {}});
    REQUIRE(search_accepted.has_value());
    REQUIRE(project_search_started);
    CHECK(searched_project == project);
    CHECK(searched_window == window);
    CHECK(search_query == "needle");

    runtime.views().set_caret(view, TextOffset{0});
    CommandLoop command_loop(runtime);
    const CommandId toggle_mark = require_command(runtime, "selection.toggle-mark");
    CHECK(command_loop.execute(toggle_mark, context).status == CommandLoopStatus::Executed);
    CHECK(runtime.views().mark(view) == std::optional<TextOffset>{TextOffset{0}});
    CHECK(message == "mark set");
    CHECK(command_loop.execute(toggle_mark, context).status == CommandLoopStatus::Executed);
    CHECK_FALSE(runtime.views().mark(view).has_value());
    CHECK(message == "mark cleared");

    runtime.views().set_selection(view, {.anchor = TextOffset{0}, .head = TextOffset{1}});
    command_loop.set_pending_prefix({.count = std::nullopt, .register_name = "a", .extra = {}});
    CHECK(command_loop.execute(require_command(runtime, "edit.copy-region"), context).status ==
          CommandLoopStatus::Executed);
    CHECK(clipboard == "a");
    CHECK(message == "copied");
    CHECK_FALSE(runtime.views().mark(view).has_value());

    CHECK(command_loop.execute(require_command(runtime, "edit.yank"), context).status ==
          CommandLoopStatus::Executed);
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() == "aabc\n");

    runtime.views().set_selection(view, {.anchor = TextOffset{0}, .head = TextOffset{1}});
    CHECK(command_loop.execute(require_command(runtime, "edit.kill-region"), context).status ==
          CommandLoopStatus::Executed);
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() == "abc\n");
    CHECK(clipboard == "a");

    runtime.views().set_caret(view, TextOffset{0});
    requested_soft_kill_range = GuileTextRange{0, 3};
    CHECK(command_loop.execute(require_command(runtime, "edit.kill-line"), context).status ==
          CommandLoopStatus::Executed);
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() == "\n");
    CHECK(clipboard == "abc");

    runtime.views().set_caret(view, TextOffset{0});
    command_loop.set_pending_prefix({.count = std::nullopt, .register_name = "a", .extra = {}});
    CHECK(command_loop.execute(require_command(runtime, "edit.yank"), context).status ==
          CommandLoopStatus::Executed);
    CHECK(runtime.buffers().get(buffer).snapshot().content().to_string() == "a\n");
    CHECK(command_loop.pending_prefix().empty());

    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.command_revision == 1);
    CHECK(snapshot.scripted_commands == 178);
    CHECK(snapshot.provider_revision == 1);
    CHECK(snapshot.scripted_providers == 6);
    CHECK_FALSE(snapshot.last_error.has_value());
}
