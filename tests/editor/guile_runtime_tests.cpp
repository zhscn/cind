#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/command_loop.hpp"
#include "editor/runtime.hpp"
#include "script/guile_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
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
(define-language-profile! host 'user.partial-language
  '((lexing . cind.c-family.lexer)) '())
(define-file-mode-rule! host 'user.partial-mode 'text-mode '(".partial") '())
(define-project-provider! host 'user.partial-project '("partial.project"))
(error "extension failed")
)");

    const std::expected<void, std::string> loaded = guile.load_extension(extension.path().string());

    REQUIRE_FALSE(loaded.has_value());
    CHECK_FALSE(runtime.commands().find("user.partial").has_value());
    CHECK_FALSE(runtime.keymaps().find("user.partial-map").has_value());
    CHECK_FALSE(runtime.languages().find_profile("user.partial-language").has_value());
    CHECK(runtime.resource_policies().file_mode_rules().empty());
    CHECK(runtime.resource_policies().project_providers().empty());
    REQUIRE(guile.set_message("extension load failed").has_value());
    REQUIRE(guile.command_feedback_state().has_value());
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

TEST_CASE("Guile minibuffer history policy deduplicates and bounds entries") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);

    const std::expected<GuileEvaluationResult, std::string> result =
        guile.evaluate({.source = R"((use-modules (cind minibuffer))
(let ((policy (make-bounded-history-policy 2)))
  (list (policy #("one" "two") "two")
        (policy #("one" "two") "three"))))",
                        .source_name = "minibuffer-history-test.scm"});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->values == std::vector<std::string>{"(#(\"one\" \"two\") #(\"two\" \"three\"))"});
}

TEST_CASE("Guile minibuffer history storage is scoped to the editor host") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);

    const std::expected<GuileEvaluationResult, std::string> result =
        guile.evaluate({.source = R"((use-modules (cind minibuffer))
(set-minibuffer-history! host "search" #("first" "second"))
(minibuffer-history host "search"))",
                        .source_name = "minibuffer-history-storage-test.scm"});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->values == std::vector<std::string>{"#(\"first\" \"second\")"});
}

TEST_CASE("Guile completion selection follows stable item ids") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);

    const std::expected<GuileEvaluationResult, std::string> result =
        guile.evaluate({.source = R"((use-modules (cind completion))
(list (reconcile-completion! host #(11 22 33))
      (move-completion-selection! host 1)
      (reconcile-completion! host #(44 22 33))
      (completion-selection host)
      (move-completion-selection! host -1)
      (completion-transition! host #() #t #t)
      (completion-transition! host #() #t #f)
      (completion-selection host)))",
                        .source_name = "completion-selection-test.scm"});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->error.has_value());
    CHECK(result->values == std::vector<std::string>{"(0 1 1 1 0 #(#f #f) #(#f #t) #f)"});
}

TEST_CASE("Guile command feedback owns message and command input state") {
    EditorRuntime runtime;
    std::size_t completion_refreshes = 0;
    bool fail_completion_refresh = false;
    GuileHostServices services;
    services.refresh_completion = [&]() -> std::expected<void, std::string> {
        ++completion_refreshes;
        if (fail_completion_refresh) {
            return std::unexpected("completion refresh failed");
        }
        return {};
    };
    GuileRuntime guile(runtime, std::move(services));

    REQUIRE(guile.command_input("C-x", true).has_value());
    REQUIRE(guile.set_message("pending").has_value());
    REQUIRE(guile
                .command_result_feedback(CommandLoopStatus::Executed, true,
                                         std::string_view("file.save"), false, "ignored")
                .has_value());
    const std::expected<GuileCommandFeedbackState, std::string> pending =
        guile.command_feedback_state();
    REQUIRE(pending.has_value());
    CHECK(pending->message == "pending");
    CHECK(pending->last_key == "C-x");
    CHECK(pending->last_command == "file.save");

    REQUIRE(
        guile
            .command_result_feedback(CommandLoopStatus::AwaitingInput, true, std::nullopt, true, {})
            .has_value());
    const std::expected<GuileCommandFeedbackState, std::string> next =
        guile.command_feedback_state();
    REQUIRE(next.has_value());
    CHECK(next->message.empty());
    CHECK(next->last_key == "C-x");
    CHECK(next->last_command == "file.save");

    REQUIRE(guile.set_message("prefix-safe").has_value());
    REQUIRE(
        guile
            .command_result_feedback(CommandLoopStatus::Prefix, true, std::nullopt, false, "prefix")
            .has_value());
    const std::expected<GuileCommandFeedbackState, std::string> prefix =
        guile.command_feedback_state();
    REQUIRE(prefix.has_value());
    CHECK(prefix->message == "prefix-safe");
    REQUIRE(guile
                .command_result_feedback(CommandLoopStatus::Disabled, true, std::nullopt, false,
                                         "disabled")
                .has_value());
    const std::expected<GuileCommandFeedbackState, std::string> disabled =
        guile.command_feedback_state();
    REQUIRE(disabled.has_value());
    CHECK(disabled->message == "disabled");
    CHECK(completion_refreshes == 4);

    fail_completion_refresh = true;
    REQUIRE(
        guile.command_result_feedback(CommandLoopStatus::Executed, true, std::nullopt, false, {})
            .has_value());
    const std::expected<GuileCommandFeedbackState, std::string> refresh_failed =
        guile.command_feedback_state();
    REQUIRE(refresh_failed.has_value());
    CHECK(refresh_failed->message == "completion refresh failed");
    CHECK(completion_refreshes == 5);
}

TEST_CASE("Guile buffer edit observers own post-edit policy") {
    EditorRuntime runtime;
    const BufferId buffer = runtime.buffers().create({.name = "edit-observer",
                                                      .initial_text = "hello",
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer, TextOffset{5});
    const RevisionId revision = runtime.buffers().get(buffer).snapshot().revision();
    std::size_t refreshes = 0;
    GuileHostServices services;
    services.interaction_mechanism_status = [=] {
        return GuileInteractionMechanismStatus{.active = true,
                                               .candidate_count = 0,
                                               .buffer = buffer,
                                               .view = view,
                                               .candidate_revision = 0};
    };
    services.refresh_interaction =
        [&](std::string_view provider) -> std::expected<void, std::string> {
        CHECK(provider == "test");
        ++refreshes;
        return {};
    };
    GuileRuntime guile(runtime, std::move(services));
    const CommandId accept = define_command(runtime, "edit-observer.accept");
    REQUIRE(guile
                .interaction_started(
                    {.kind = InteractionKind::Picker,
                     .keymap = "interaction.picker",
                     .input_state = "emacs",
                     .buffer_name = " *minibuffer*",
                     .prompt = "Pick: ",
                     .initial_input = {},
                     .history = {},
                     .provider = "test",
                     .allow_custom_input = false,
                     .accept_command = accept,
                     .arguments = {}},
                    {.window = runtime.windows().create(view), .buffer = buffer, .view = view})
                .has_value());
    const std::expected<std::optional<GuileInteractionPolicyState>, std::string> policy =
        guile.interaction_policy_state();
    REQUIRE(policy.has_value());
    REQUIRE(policy->has_value());
    CHECK((*policy)->provider == "test");
    CHECK((*policy)->prompt == "Pick: ");
    const std::expected<void, std::string> installed = guile.install_buffer_lifecycle_policies();
    INFO(installed.error_or(""));
    REQUIRE(installed.has_value());

    REQUIRE(guile.set_message("pending").has_value());
    REQUIRE(guile.buffer_edited(buffer, view, revision).has_value());
    const std::expected<GuileCommandFeedbackState, std::string> cleared =
        guile.command_feedback_state();
    REQUIRE(cleared.has_value());
    CHECK(cleared->message.empty());
    CHECK(refreshes == 1);

    const std::expected<GuileEvaluationResult, std::string> observed =
        guile.evaluate({.source = R"((use-modules (cind command) (cind lifecycle))
(observe-buffer-edits!
 host
 (lambda (host buffer view revision)
   (set-message! host (number->string revision))))
#t)",
                        .source_name = "buffer-edit-observer-test.scm"});
    REQUIRE(observed.has_value());
    CHECK_FALSE(observed->error.has_value());
    REQUIRE(guile.buffer_edited(buffer, view, revision).has_value());
    const std::expected<GuileCommandFeedbackState, std::string> feedback =
        guile.command_feedback_state();
    REQUIRE(feedback.has_value());
    CHECK(feedback->message == std::to_string(revision));
    CHECK(refreshes == 2);
}

TEST_CASE("Guile language profile declarations replace configuration atomically") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);

    const std::expected<GuileEvaluationResult, std::string> defined =
        guile.evaluate({.source = R"((define-language-profile! host 'user.atomic
  '((lexing . cind.c-family.lexer))
  '((language.c-family.dialect . "c"))))",
                        .source_name = "language-profile-test.scm"});
    REQUIRE(defined.has_value());
    CHECK_FALSE(defined->error.has_value());

    const std::optional<LanguageProfileId> profile_id =
        runtime.languages().find_profile("user.atomic");
    REQUIRE(profile_id.has_value());
    const LanguageProfileId profile = *profile_id;
    const std::optional<SettingId> dialect_id =
        runtime.setting_definitions().find("language.c-family.dialect");
    REQUIRE(dialect_id.has_value());
    const SettingId dialect = *dialect_id;
    REQUIRE(runtime.languages().profile(profile).provider(LanguageFacet::Lexing).has_value());
    REQUIRE(runtime.languages().profile(profile).defaults.find(dialect) != nullptr);
    CHECK(std::get<std::string>(*runtime.languages().profile(profile).defaults.find(dialect)) ==
          "c");

    const std::expected<GuileEvaluationResult, std::string> rejected =
        guile.evaluate({.source = R"((define-language-profile! host 'user.atomic
  '((syntax . cind.c-family.syntax))
  '((language.c-family.dialect . #t))))",
                        .source_name = "language-profile-test.scm"});
    REQUIRE(rejected.has_value());
    REQUIRE(rejected->error.has_value());

    const LanguageRegistry::ProfileDefinition& unchanged = runtime.languages().profile(profile);
    CHECK(unchanged.provider(LanguageFacet::Lexing).has_value());
    CHECK_FALSE(unchanged.provider(LanguageFacet::Syntax).has_value());
    REQUIRE(unchanged.defaults.find(dialect) != nullptr);
    CHECK(std::get<std::string>(*unchanged.defaults.find(dialect)) == "c");
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

TEST_CASE("Guile interaction providers transform cancellable async host results") {
    AsyncRuntime async;
    AsyncScriptHost async_host(async);
    EditorRuntime runtime;
    GuileRuntime guile(runtime, async_services(async_host));
    REQUIRE(guile.install_core_providers().has_value());

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "cind-guile-files-provider-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory / "nested");
    {
        std::ofstream file(directory / "example.cpp", std::ios::binary | std::ios::trunc);
        file << "int value;\n";
    }

    const BufferId buffer =
        runtime.buffers().create({.name = "current.cpp",
                                  .initial_text = {},
                                  .kind = BufferKind::File,
                                  .resource_uri = (directory / "current.cpp").string(),
                                  .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);
    InteractionProviderResult result =
        runtime.interaction_providers().complete("files", context, "");
    auto* provider_task = std::get_if<InteractionCandidateAsync>(&result);
    REQUIRE(provider_task != nullptr);

    bool completed = false;
    std::string failure;
    std::vector<InteractionCandidate> candidates;
    std::expected<InteractionCandidateAsync::Cancel, std::string> started = provider_task->start(
        [&](std::vector<InteractionCandidate> values) {
            candidates = std::move(values);
            completed = true;
        },
        [&](std::string message) { failure = std::move(message); }, [] {});
    REQUIRE(started.has_value());
    CHECK(guile.snapshot().outstanding_async_tasks == 1);
    drain_until(async, [&] { return completed || !failure.empty(); });

    CHECK(failure.empty());
    CHECK(guile.snapshot().outstanding_async_tasks == 0);
    CHECK(std::ranges::any_of(candidates, [&](const InteractionCandidate& candidate) {
        return candidate.value == (directory / "example.cpp").string() &&
               candidate.label == "example.cpp" && candidate.detail == directory.string();
    }));
    CHECK(std::ranges::any_of(candidates, [&](const InteractionCandidate& candidate) {
        return candidate.value ==
                   (directory / "nested").string() + std::filesystem::path::preferred_separator &&
               candidate.label ==
                   std::string("nested") + std::filesystem::path::preferred_separator;
    }));
    CHECK(std::ranges::any_of(candidates, [&](const InteractionCandidate& candidate) {
        return candidate.label == "../" &&
               candidate.value ==
                   directory.parent_path().string() + std::filesystem::path::preferred_separator;
    }));

    completed = false;
    candidates.clear();
    result = runtime.interaction_providers().complete(
        "files", context, directory.string() + std::filesystem::path::preferred_separator);
    provider_task = std::get_if<InteractionCandidateAsync>(&result);
    REQUIRE(provider_task != nullptr);
    started = provider_task->start(
        [&](std::vector<InteractionCandidate> values) {
            candidates = std::move(values);
            completed = true;
        },
        [&](std::string message) { failure = std::move(message); }, [] {});
    REQUIRE(started.has_value());
    drain_until(async, [&] { return completed || !failure.empty(); });
    CHECK(failure.empty());
    CHECK(std::ranges::any_of(candidates, [&](const InteractionCandidate& candidate) {
        return candidate.label == "../" &&
               candidate.value ==
                   directory.parent_path().string() + std::filesystem::path::preferred_separator;
    }));

    completed = false;
    candidates.clear();
    result = runtime.interaction_providers().complete("files", context, "exa");
    provider_task = std::get_if<InteractionCandidateAsync>(&result);
    REQUIRE(provider_task != nullptr);
    started = provider_task->start(
        [&](std::vector<InteractionCandidate> values) {
            candidates = std::move(values);
            completed = true;
        },
        [&](std::string message) { failure = std::move(message); }, [] {});
    REQUIRE(started.has_value());
    drain_until(async, [&] { return completed || !failure.empty(); });
    CHECK(failure.empty());
    CHECK(std::ranges::any_of(candidates, [&](const InteractionCandidate& candidate) {
        return candidate.value == (directory / "example.cpp").string();
    }));
    std::filesystem::remove_all(directory, ignored);
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

TEST_CASE("Ares REPL participates in the command loop") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);
    REQUIRE(guile.install_core_commands().has_value());
    REQUIRE(guile.install_core_providers().has_value());
    const BufferId buffer = runtime.buffers().create({.name = "ares-test",
                                                      .initial_text = {},
                                                      .kind = BufferKind::Scratch,
                                                      .resource_uri = std::nullopt,
                                                      .read_only = false});
    const ViewId view = runtime.views().create(buffer);
    const WindowId window = runtime.windows().create(view);
    CommandContext context(runtime, window, buffer, view);

    const CommandResult requested = runtime.commands().invoke(
        require_command(runtime, "scheme.eval-expression"), context, CommandInvocation{});
    REQUIRE(requested.has_value());
    const auto* interaction = std::get_if<InteractionRequest>(&*requested);
    REQUIRE(interaction != nullptr);
    CHECK(interaction->kind == InteractionKind::Picker);
    CHECK(interaction->provider == "scheme-repl");
    CHECK(interaction->allow_custom_input);

    const std::vector<InteractionCandidate> candidates =
        complete_provider(runtime, "scheme-repl", context, "(str");
    CHECK(std::ranges::any_of(candidates, [](const InteractionCandidate& candidate) {
        return candidate.value == "(string-append" && candidate.label == "string-append" &&
               candidate.detail.find("function") != std::string::npos;
    }));
    CHECK(complete_provider(runtime, "scheme-repl", context, "(+ 1 2)").empty());

    CHECK_FALSE(runtime.commands().find("scheme.ares-start").has_value());
    CHECK_FALSE(runtime.commands().find("scheme.ares-status").has_value());
    CHECK_FALSE(runtime.commands().find("scheme.ares-stop").has_value());

    const auto defined =
        guile.evaluate({.source = "(define ares-value 41)", .source_name = "ares-repl-test.scm"});
    REQUIRE(defined.has_value());
    CHECK_FALSE(defined->error.has_value());
    const auto evaluated =
        guile.evaluate({.source = "(+ ares-value 1)", .source_name = "ares-repl-test.scm"});
    REQUIRE(evaluated.has_value());
    CHECK_FALSE(evaluated->error.has_value());
    CHECK(evaluated->values == std::vector<std::string>{"42"});
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
    const CommandId location_visit = define_command(runtime, "location.visit");
    const CommandId location_next = define_command(runtime, "location.next");
    const CommandId location_previous = define_command(runtime, "location.previous");
    const CommandId window_dismiss = define_command(runtime, "window.dismiss");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> installed = guile.install_default_keymaps();

    REQUIRE(installed.has_value());
    CHECK(*installed == 21);
    const KeymapId editor = require_keymap(runtime, "editor.default");
    const KeymapId application = require_keymap(runtime, "application.global");
    const KeymapId control_x = require_keymap(runtime, "editor.control-x");
    const KeymapId system = require_keymap(runtime, "editor.system");
    const KeymapId interaction_text = require_keymap(runtime, "interaction.text");
    const KeymapId interaction_picker = require_keymap(runtime, "interaction.picker");
    const KeymapId location_list = require_keymap(runtime, "cind.location-list.map");
    const KeymapId policy_created_window = require_keymap(runtime, "window.policy-created");
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
    CHECK(resolve_command(runtime, location_list, "RET") == location_visit);
    CHECK(resolve_command(runtime, location_list, "M-n") == location_next);
    CHECK(resolve_command(runtime, location_list, "M-p") == location_previous);
    CHECK(resolve_command(runtime, policy_created_window, "q") == window_dismiss);
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
    CHECK(snapshot.modules ==
          std::vector<std::string>{
              "cind application", "cind command",    "cind completion",  "cind input",
              "cind lsp",         "cind async",      "cind workbench",   "cind lifecycle",
              "cind pointer",     "cind extension",  "cind emacs",       "cind toy-modal",
              "cind meow",        "cind vim",        "cind helix",       "cind structural",
              "cind paredit",     "cind minibuffer", "cind development", "cind ares",
              "cind introspect",  "cind core"});
    CHECK(snapshot.binding_revision == 1);
    CHECK_FALSE(snapshot.last_error.has_value());
}

TEST_CASE("Guile display policy resolves deterministic slots and can be replaced") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);
    REQUIRE(guile.install_display_policy().has_value());
    const WindowId active{0, 1};
    const WindowId adjacent{1, 1};
    GuileDisplayFacts facts{.intent = "jump",
                            .origin = active,
                            .active = active,
                            .windows = {{.window = active,
                                         .role = std::nullopt,
                                         .pinned = true,
                                         .created_by_policy = false},
                                        {.window = adjacent,
                                         .role = std::nullopt,
                                         .pinned = false,
                                         .created_by_policy = false}},
                            .slots = {}};
    std::expected<GuileDisplayPlan, std::string> plan = guile.display_plan(facts);
    REQUIRE(plan.has_value());
    CHECK(plan->action == GuileDisplayPlan::Action::Reuse);
    CHECK(plan->target == adjacent);

    facts.intent = "tools";
    plan = guile.display_plan(facts);
    REQUIRE(plan.has_value());
    CHECK(plan->action == GuileDisplayPlan::Action::Split);
    CHECK(plan->target == active);
    CHECK(plan->axis == WindowSplitAxis::Rows);
    CHECK(plan->ratio == doctest::Approx(0.72F));
    CHECK(plan->role == std::optional<std::string>{"tools"});

    facts.slots = {{.role = "tools", .window = adjacent}};
    plan = guile.display_plan(facts);
    REQUIRE(plan.has_value());
    CHECK(plan->action == GuileDisplayPlan::Action::Reuse);
    CHECK(plan->target == adjacent);

    const std::expected<GuileEvaluationResult, std::string> configured =
        guile.evaluate({.source = R"((use-modules (cind command))
(configure-display-policy!
 host
 (lambda (host facts)
   (vector 'display-reuse (vector-ref facts 2))))
)",
                        .source_name = "display-policy-test.scm"});
    REQUIRE(configured.has_value());
    CHECK_FALSE(configured->error.has_value());
    facts.intent = "doc";
    plan = guile.display_plan(facts);
    REQUIRE(plan.has_value());
    CHECK(plan->action == GuileDisplayPlan::Action::Reuse);
    CHECK(plan->target == active);
}

TEST_CASE("Guile keymap policy treats unavailable commands as optional") {
    EditorRuntime runtime;
    const CommandId save = define_command(runtime, "file.save");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> first = guile.install_default_keymaps();
    const std::expected<std::size_t, std::string> second = guile.install_default_keymaps();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 3);
    CHECK(*second == 3);
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

    INFO((first ? std::string{} : first.error()));
    INFO((second ? std::string{} : second.error()));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == 8);
    CHECK(*second == 8);
    const ModeId fundamental = runtime.modes().find("fundamental-mode").value_or(ModeId{});
    const ModeId prog = runtime.modes().find("prog-mode").value_or(ModeId{});
    const ModeId special = runtime.modes().find("special-mode").value_or(ModeId{});
    const ModeId scheme = runtime.modes().find("scheme-mode").value_or(ModeId{});
    const ModeId location_list = runtime.modes().find("cind.location-list").value_or(ModeId{});
    const ModeId cpp = runtime.modes().find("cind.cpp").value_or(ModeId{});
    REQUIRE(fundamental);
    REQUIRE(prog);
    REQUIRE(special);
    REQUIRE(scheme);
    REQUIRE(location_list);
    REQUIRE(cpp);
    CHECK(runtime.modes().definition(prog).parent == fundamental);
    CHECK(runtime.modes().definition(prog).things ==
          std::vector<ModeThingBinding>{{.name = "word", .definition = "cind.word"},
                                        {.name = "symbol", .definition = "cind.symbol"}});
    CHECK(runtime.modes().definition(fundamental).completion_providers ==
          std::optional<std::vector<std::string>>{{"word", "path"}});
    CHECK(runtime.things().find("cind.angle").has_value());
    CHECK(runtime.things().find("cind.defun").has_value());
    CHECK(runtime.motions().find("cind.forward-word").has_value());
    CHECK(runtime.motions().find("cind.forward-symbol").has_value());
    CHECK(runtime.modes().definition(special).parent == fundamental);
    CHECK(runtime.modes().definition(special).interaction_class == InteractionClass::Interface);
    CHECK(runtime.modes().definition(scheme).parent == prog);
    const std::optional<LanguageProfileId> scheme_language =
        runtime.languages().find_profile("cind.scheme");
    REQUIRE(scheme_language.has_value());
    CHECK(runtime.modes().definition(scheme).language == scheme_language);
    CHECK(runtime.languages()
              .profile(*scheme_language)
              .provider(LanguageFacet::StructuralMotion)
              .has_value());
    CHECK(runtime.languages()
              .profile(*scheme_language)
              .provider(LanguageFacet::StructuralEditing)
              .has_value());
    CHECK(runtime.languages()
              .profile(*scheme_language)
              .provider(LanguageFacet::Indentation)
              .has_value());
    CHECK(runtime.languages()
              .profile(*scheme_language)
              .provider(LanguageFacet::Highlighting)
              .has_value());
    CHECK(runtime.modes().definition(scheme).keymaps ==
          std::vector<KeymapId>{require_keymap(runtime, "scheme-mode-map")});
    CHECK(runtime.modes().definition(scheme).completion_providers ==
          std::optional<std::vector<std::string>>{{"ares", "word"}});
    CHECK(runtime.modes().definition(scheme).completion_auto == std::optional<bool>{true});
    CHECK_FALSE(runtime.modes().definition(prog).completion_auto.has_value());
    CHECK_FALSE(runtime.modes().definition(cpp).completion_auto.has_value());
    CHECK(runtime.modes().definition(location_list).parent == special);
    CHECK_FALSE(runtime.modes().definition(location_list).language.has_value());
    CHECK(runtime.modes().definition(location_list).keymaps ==
          std::vector<KeymapId>{require_keymap(runtime, "cind.location-list.map")});
    const std::optional<LanguageProfileId> cpp_language =
        runtime.languages().find_profile("cind.cpp");
    REQUIRE(cpp_language.has_value());
    CHECK(runtime.modes().definition(cpp).language == cpp_language);
    CHECK(runtime.languages().profile(*cpp_language).provider(LanguageFacet::Lexing).has_value());
    CHECK(runtime.languages()
              .profile(*cpp_language)
              .provider(LanguageFacet::StructuralMotion)
              .has_value());
    CHECK(runtime.languages()
              .profile(*cpp_language)
              .provider(LanguageFacet::StructuralEditing)
              .has_value());
    CHECK(runtime.modes().definition(cpp).things ==
          std::vector<ModeThingBinding>{{.name = "angle", .definition = "cind.angle"},
                                        {.name = "defun", .definition = "cind.defun"},
                                        {.name = "string", .definition = "cind.string"}});
    CHECK(runtime.modes().definition(cpp).completion_providers ==
          std::optional<std::vector<std::string>>{{"clangd", "word", "path"}});
    const InputStrategyId emacs_strategy =
        runtime.input_strategies().find("emacs").value_or(InputStrategyId{});
    REQUIRE(emacs_strategy);
    CHECK(runtime.input_strategies().default_strategy() == emacs_strategy);
    CHECK(guile.snapshot().mode_revision == 2);
    CHECK(guile.snapshot().scripted_modes == 8);
}

TEST_CASE("bundled Guile policy defines file modes and project discovery providers") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);
    REQUIRE(guile.install_input_states().has_value());
    REQUIRE(guile.install_core_modes().has_value());
    const ModeId cpp = runtime.modes().find("cind.cpp").value_or(ModeId{});
    REQUIRE(cpp);

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

TEST_CASE("bundled Guile startup policy produces validated bootstrap plans") {
    EditorRuntime runtime;
    GuileRuntime guile(runtime);
    REQUIRE(guile.install_input_states().has_value());
    REQUIRE(guile.install_core_modes().has_value());
    REQUIRE(guile.install_core_resource_policies().has_value());
    REQUIRE(guile.install_buffer_lifecycle_policies().has_value());

    const ModeId fundamental = runtime.modes().find("fundamental-mode").value_or(ModeId{});
    const ModeId cpp = runtime.modes().find("cind.cpp").value_or(ModeId{});
    REQUIRE(fundamental);
    REQUIRE(cpp);

    const std::expected<StartupPlan, std::string> scratch =
        guile.startup_plan({.requested_resource = {}, .has_initial_text = false});
    REQUIRE(scratch.has_value());
    CHECK(scratch->buffer.name == "*scratch*");
    CHECK(scratch->buffer.kind == BufferKind::Scratch);
    CHECK(scratch->buffer.major_mode == fundamental);
    CHECK_FALSE(scratch->buffer.use_initial_text);
    CHECK(scratch->style == CppIndentStyle{});
    CHECK(scratch->style_origin == "plain text");
    CHECK_FALSE(scratch->resource_to_open.has_value());
    CHECK_FALSE(scratch->startup_placeholder);

    const std::filesystem::path requested = "src/main.cpp";
    const std::string normalized = std::filesystem::absolute(requested).lexically_normal().string();
    const std::expected<StartupPlan, std::string> preloaded =
        guile.startup_plan({.requested_resource = requested.string(), .has_initial_text = true});
    REQUIRE(preloaded.has_value());
    CHECK(preloaded->buffer.name == "main.cpp");
    CHECK(preloaded->buffer.kind == BufferKind::File);
    CHECK(preloaded->buffer.resource == normalized);
    CHECK(preloaded->buffer.major_mode == cpp);
    CHECK(preloaded->buffer.use_initial_text);
    CHECK(preloaded->style == CppIndentStyle{});
    CHECK(preloaded->style_origin == "llvm (fallback)");
    CHECK_FALSE(preloaded->resource_to_open.has_value());
    CHECK_FALSE(preloaded->startup_placeholder);

    const std::expected<StartupPlan, std::string> deferred =
        guile.startup_plan({.requested_resource = requested.string(), .has_initial_text = false});
    REQUIRE(deferred.has_value());
    CHECK(deferred->buffer.name == "*scratch*");
    CHECK(deferred->buffer.kind == BufferKind::Scratch);
    CHECK(deferred->buffer.major_mode == fundamental);
    CHECK(deferred->style == CppIndentStyle{});
    CHECK(deferred->style_origin == "plain text");
    CHECK(deferred->resource_to_open == normalized);
    CHECK(deferred->startup_placeholder);

    const std::expected<SessionPlan, std::string> session =
        guile.session_plan({.has_initial_text = true});
    REQUIRE(session.has_value());
    CHECK(session->buffer.name == "*session*");
    CHECK(session->buffer.kind == BufferKind::Scratch);
    CHECK(session->buffer.major_mode == cpp);
    CHECK(session->buffer.use_initial_text);
    CHECK_FALSE(session->buffer.resource.has_value());
    CHECK_FALSE(session->buffer.read_only);

    const std::expected<GuileEvaluationResult, std::string> configured =
        guile.evaluate({.source = R"((use-modules (cind lifecycle))
(configure-session-policy!
 host
 (lambda (host facts)
   #(session-plan
     #(startup-buffer "generated-session" empty generated #f #t fundamental-mode))))
)",
                        .source_name = "session-policy-test.scm"});
    REQUIRE(configured.has_value());
    CHECK_FALSE(configured->error.has_value());

    const std::expected<SessionPlan, std::string> overridden =
        guile.session_plan({.has_initial_text = true});
    REQUIRE(overridden.has_value());
    CHECK(overridden->buffer.name == "generated-session");
    CHECK(overridden->buffer.kind == BufferKind::Generated);
    CHECK(overridden->buffer.major_mode == fundamental);
    CHECK_FALSE(overridden->buffer.use_initial_text);
    CHECK_FALSE(overridden->buffer.resource.has_value());
    CHECK(overridden->buffer.read_only);
}

TEST_CASE("bundled Guile commands return editor command actions") {
    EditorRuntime runtime;
    const ModeId fundamental_mode = runtime.modes().define("fundamental-mode", ModeKind::Major);
    const ModeId special_mode = runtime.modes().define("special-mode", ModeKind::Major);
    const ModeId cpp_mode = runtime.modes().define("cind.cpp", ModeKind::Major);
    runtime.modes().set_completion_providers(cpp_mode, {{"clangd"}});

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
    const ViewId alternate_view = runtime.views().create(other);
    const WindowId alternate_window = runtime.windows().create(alternate_view);
    CommandContext context(runtime, window, buffer, view);

    std::pair<WindowId, BufferId> displayed;
    bool buffer_displayed = false;
    WindowId displayed_help_window;
    std::string help_name;
    std::string help_text;
    ModeId help_mode;
    std::string help_style_origin;
    std::tuple<ViewId, std::uint32_t, std::uint32_t> moved;
    bool caret_moved = false;
    ProjectId indexed_project;
    bool project_index_requested = false;
    std::optional<ScriptAsyncRequest> pending_async_request;
    ScriptAsyncCallbacks pending_async_callbacks;
    std::vector<ScriptAsyncRequest> async_requests;
    std::vector<std::uint64_t> cancelled_async_tasks;
    std::uint64_t next_async_task = 1;
    std::optional<CommandTarget> ensured_lsp_target;
    std::optional<ScriptLspProviderSpec> ensured_lsp_provider;
    std::size_t lsp_sessions_ensured = 0;
    std::vector<std::uint64_t> attached_lsp_sessions;
    std::vector<std::pair<BufferId, std::uint64_t>> synchronized_lsp_sessions;
    std::optional<GuileBufferCreation> created_buffer;
    bool only_buffer = false;
    BufferId released_buffer;
    BufferId replacement_buffer;
    bool buffer_released = false;
    std::string release_error;
    WindowId split_target;
    WindowSplitAxis split_axis = WindowSplitAxis::Rows;
    bool window_split = false;
    WindowId deleted_window;
    bool window_deleted = false;
    WindowId retained_window;
    bool other_windows_deleted = false;
    WindowId focused_window;
    bool other_window_selected = false;
    std::string window_error;
    std::string clipboard;
    std::string clipboard_error;
    std::optional<GuileTextRange> requested_soft_kill_range;
    bool requested_structural_kill = false;
    std::optional<std::uint32_t> requested_motion_target;
    std::optional<ViewId> requested_motion_view;
    std::optional<ViewSelection> requested_motion_source;
    std::optional<std::string> requested_motion;
    std::int64_t requested_motion_count = 0;
    bool requested_motion_extend = false;
    const WorkbenchId workbench{0, 1};
    const std::string workbench_session = "serialized workbench session";
    std::optional<std::string> restored_workbench_session;
    GuileRuntime guile(
        runtime,
        {.display_buffer =
             [&](WindowId target_window, BufferId target_buffer, std::string_view,
                 std::optional<GuileDisplayPosition>) -> std::expected<WindowId, std::string> {
             displayed = std::pair{target_window, target_buffer};
             buffer_displayed = true;
             return target_window;
         },
         .display_generated_buffer = [&](WindowId target_window, std::string name, std::string text,
                                         ModeId mode, std::string_view style_origin,
                                         std::string_view) -> std::expected<WindowId, std::string> {
             displayed_help_window = target_window;
             help_name = std::move(name);
             help_text = std::move(text);
             help_mode = mode;
             help_style_origin = style_origin;
             return target_window;
         },
         .navigate_jump = {},
         .mark_jump = {},
         .visit_jump = {},
         .link_jump = {},
         .jump_branches = {},
         .jump_node = {},
         .evict_jumps = {},
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
         .scroll_view_lines = {},
         .move_caret_line_boundary = {},
         .delete_grapheme = {},
         .newline = {},
         .indent = {},
         .type_text = {},
         .structural_edit = {},
         .interaction_mechanism_status = {},
         .interaction_origin_project = {},
         .refresh_interaction = {},
         .submit_interaction = {},
         .replace_interaction_input = {},
         .cancel_interaction = {},
         .completion_active = {},
         .refresh_completion = {},
         .ensure_lsp_session = [&](CommandTarget target, ScriptLspProviderSpec provider)
             -> std::expected<std::uint64_t, std::string> {
             ++lsp_sessions_ensured;
             ensured_lsp_target = target;
             ensured_lsp_provider = std::move(provider);
             return 41;
         },
         .attach_lsp_diagnostics = [&](std::uint64_t session) -> std::expected<void, std::string> {
             attached_lsp_sessions.push_back(session);
             return {};
         },
         .synchronize_lsp_session = [&](BufferId target,
                                        std::uint64_t session) -> std::expected<void, std::string> {
             synchronized_lsp_sessions.emplace_back(target, session);
             return {};
         },
         .start_completion = {},
         .focus_completion = {},
         .apply_completion = {},
         .cancel_completion = {},
         .cancel_pending_input = {},
         .view_position = {},
         .view_line_prefix = {},
         .view_syntax_token = {},
         .view_identifier_words = {},
         .publish_location_list = {},
         .location_target = {},
         .position_buffer_view = {},
         .request_project_index = [&](ProjectId target) -> std::expected<void, std::string> {
             indexed_project = target;
             project_index_requested = true;
             return {};
         },
         .open_buffers =
             [&] {
                 return only_buffer ? std::vector<BufferId>{buffer}
                                    : std::vector<BufferId>{buffer, other};
             },
         .create_workbench = [](std::string, std::optional<ProjectId>)
             -> std::expected<WorkbenchId, std::string> { return WorkbenchId{1, 1}; },
         .switch_workbench = [](WorkbenchId) -> std::expected<void, std::string> { return {}; },
         .close_workbench = [](WorkbenchId) -> std::expected<void, std::string> { return {}; },
         .workbench_session_state = [&] { return workbench_session; },
         .prepare_workbench_session_restore =
             [&](std::string_view state) -> std::expected<GuileWorkbenchRestorePlan, std::string> {
             restored_workbench_session = state;
             if (state == "resource session") {
                 return GuileWorkbenchRestorePlan{
                     .resources = {{.resource = "/tmp/restored.cc", .targets = {}}}, .mru = {}};
             }
             return GuileWorkbenchRestorePlan{};
         },
         .show_buffer_in_window = [](WindowId, BufferId, std::uint32_t)
             -> std::expected<void, std::string> { return {}; },
         .window_buffer = [=](WindowId) { return buffer; },
         .create_buffer = [&](GuileBufferCreation spec) -> std::expected<BufferId, std::string> {
             created_buffer = std::move(spec);
             return other;
         },
         .release_buffer = [&](BufferId target_buffer,
                               BufferId replacement) -> std::expected<void, std::string> {
             if (!release_error.empty()) {
                 return std::unexpected(release_error);
             }
             released_buffer = target_buffer;
             replacement_buffer = replacement;
             buffer_released = true;
             return {};
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
         .delete_other_windows = [&](WindowId retained) -> std::expected<void, std::string> {
             if (!window_error.empty()) {
                 return std::unexpected(window_error);
             }
             retained_window = retained;
             other_windows_deleted = true;
             return {};
         },
         .open_windows = [&] { return std::vector<WindowId>{window, alternate_window}; },
         .focus_window = [&](WindowId target) -> std::expected<void, std::string> {
             if (!window_error.empty()) {
                 return std::unexpected(window_error);
             }
             focused_window = target;
             other_window_selected = true;
             return {};
         },
         .active_key_bindings =
             [] {
                 return std::vector<GuileKeyBindingSummary>{
                     {.keys = "C-x C-s", .command = "file.save"}};
             },
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
         .soft_kill_range = [&](ViewId, bool structural)
             -> std::expected<std::optional<GuileTextRange>, std::string> {
             requested_structural_kill = structural;
             return requested_soft_kill_range;
         },
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
         .start_async_task = [&](ScriptAsyncRequest request, ScriptAsyncCallbacks callbacks)
             -> std::expected<std::uint64_t, std::string> {
             async_requests.push_back(request);
             pending_async_request = std::move(request);
             pending_async_callbacks = std::move(callbacks);
             return next_async_task++;
         },
         .cancel_async_task =
             [&](std::uint64_t task) {
                 cancelled_async_tasks.push_back(task);
                 return true;
             },
         .async_tasks = [] { return std::vector<ScriptAsyncTaskSummary>{}; }});
    REQUIRE(guile.application_state().has_value());
    CHECK(guile.application_state()->page_rows == 1);
    CHECK_FALSE(guile.set_page_rows(0).has_value());
    REQUIRE(guile.set_page_rows(17).has_value());
    CHECK(guile.application_state()->page_rows == 17);
    REQUIRE(guile.install_buffer_lifecycle_policies().has_value());
    REQUIRE(guile.buffer_created(buffer, "test style").has_value());
    CHECK(guile.buffer_style_origin(buffer).value_or("") == "test style");
    const BufferId visitor{buffer.slot + 1, buffer.generation};
    REQUIRE(guile.workbench_created(workbench, "code", window, buffer, {}).has_value());
    CHECK(guile.active_workbench().value_or(WorkbenchId{}) == workbench);
    CHECK(guile.workbench_active_window(workbench).value_or(WindowId{}) == window);
    const std::expected<GuileEvaluationResult, std::string> location_policy =
        guile.evaluate({.source = R"((use-modules (cind workbench))
(workbench-location-list-published! host '#(0 1) 1 '#(0 1) 3)
(workbench-location-list-published! host '#(0 1) 2 '#(1 1) 2)
(workbench-set-location-navigation! host '#(0 1) '#(0 1) 1)
(workbench-move-location-list! host '#(0 1) 1))",
                        .source_name = "location-policy-test.scm"});
    REQUIRE(location_policy.has_value());
    REQUIRE_FALSE(location_policy->error.has_value());
    const GuileLocationNavigation location_navigation =
        guile.workbench_location_navigation(workbench).value();
    CHECK(location_navigation.buffer == std::optional{other});
    CHECK_FALSE(location_navigation.selected_index.has_value());
    CHECK(location_navigation.location_count == 2);
    const std::vector<GuileLocationListPolicyState> location_states =
        guile.workbench_location_list_states(workbench).value();
    REQUIRE(location_states.size() == 2);
    CHECK(location_states[0].list == 1);
    CHECK(location_states[0].selected_index == 1);
    CHECK_FALSE(location_states[0].current);
    CHECK(location_states[1].list == 2);
    CHECK(location_states[1].current);
    CHECK_FALSE(guile.workbench_transaction_group_movable(workbench, 99, false).value_or(true));
    REQUIRE(guile.workbench_transaction_group_recorded(workbench, 1).has_value());
    CHECK(guile.workbench_transaction_group_movable(workbench, 1, false).value_or(false));
    CHECK_FALSE(guile.workbench_transaction_group_movable(workbench, 1, true).value_or(true));
    REQUIRE(guile.workbench_transaction_group_moved(workbench, 1, false, false).has_value());
    CHECK(guile.workbench_transaction_group_movable(workbench, 1, false).value_or(false));
    REQUIRE(guile.workbench_transaction_group_moved(workbench, 1, false, true).has_value());
    CHECK_FALSE(guile.workbench_transaction_group_movable(workbench, 1, false).value_or(true));
    CHECK(guile.workbench_transaction_group_movable(workbench, 1, true).value_or(false));
    REQUIRE(guile.workbench_transaction_group_moved(workbench, 1, true, true).has_value());
    CHECK(guile.workbench_transaction_group_movable(workbench, 1, false).value_or(false));
    REQUIRE(guile.workbench_jump_record(window, 1).value_or(false));
    REQUIRE(guile.workbench_jump_record(window, 2).value_or(false));
    REQUIRE(guile.workbench_jump_record(window, 3).value_or(false));
    CHECK(guile.workbench_jump_move(window, -2).value_or(std::nullopt) ==
          std::optional<std::uint64_t>{1});
    REQUIRE(guile.workbench_jump_record(window, 4).value_or(false));
    GuileJumpWalkState jump_walk = guile.workbench_jump_walk(window).value();
    CHECK(jump_walk.entries == std::vector<std::uint64_t>{1, 2, 3, 4});
    CHECK(jump_walk.cursor == std::optional<std::size_t>{3});
    CHECK_FALSE(guile.workbench_jump_move(window, std::numeric_limits<std::int64_t>::min())
                    .value_or(std::nullopt)
                    .has_value());
    CHECK_FALSE(guile.workbench_jump_move(window, 1).value_or(std::nullopt).has_value());
    REQUIRE(guile.workbench_jump_restore(window, {1, 2, 3, 4}, 2).has_value());
    REQUIRE(guile.workbench_jump_forget(workbench, {2, 3}).has_value());
    jump_walk = guile.workbench_jump_walk(window).value();
    CHECK(jump_walk.entries == std::vector<std::uint64_t>{1, 4});
    CHECK(jump_walk.cursor == std::optional<std::size_t>{0});
    CHECK(guile.workbench_jump_move(window, 1).value_or(std::nullopt) ==
          std::optional<std::uint64_t>{4});
    REQUIRE(guile.workbench_jump_restore(window, {1, 2, 3, 4, 5, 6}, 4).has_value());
    const GuileJumpWalkState session_walk =
        guile.workbench_jump_session_walk(window, {2, 3, 5, 6}, 4).value();
    CHECK(session_walk.entries == std::vector<std::uint64_t>{3, 5, 6});
    CHECK(session_walk.cursor == std::optional<std::size_t>{1});
    CHECK(guile.workbench_jump_track_intent("edit").value_or(false));
    CHECK_FALSE(guile.workbench_jump_track_intent("replay").value_or(true));
    REQUIRE(guile.workbench_jump_restore(window, {}, std::nullopt).has_value());
    CHECK(guile.workbench_jump_transition(window, "definition", 10, 11).value_or(std::nullopt) ==
          std::optional<std::string>{"def"});
    jump_walk = guile.workbench_jump_walk(window).value();
    CHECK(jump_walk.entries == std::vector<std::uint64_t>{10, 11});
    CHECK(jump_walk.cursor == std::optional<std::size_t>{1});
    CHECK_FALSE(guile.workbench_jump_transition(window, "replay", 12, 13)
                    .value_or(std::optional<std::string>{"unexpected"})
                    .has_value());
    CHECK(guile.workbench_jump_walk(window).value().entries == std::vector<std::uint64_t>{10, 11});
    {
        EditorRuntime parallel_runtime;
        GuileRuntime parallel(parallel_runtime);
        CHECK(guile.active_workbench().value_or(WorkbenchId{}) == workbench);
        CHECK(guile.workbench_active_window(workbench).value_or(WindowId{}) == window);
    }
    CHECK(guile.workbench_name(workbench).value_or("") == "code");
    CHECK(guile.workbench_find_by_name("code").value_or(std::optional<WorkbenchId>{}) ==
          std::optional{workbench});
    CHECK_FALSE(
        guile.workbench_find_by_name("notes").value_or(std::optional<WorkbenchId>{}).has_value());
    CHECK(guile.workbench_rename(workbench, "notes").value_or(false));
    CHECK(guile.workbench_name(workbench).value_or("") == "notes");
    const WorkbenchId other_workbench{1, 1};
    REQUIRE(guile.workbench_created(other_workbench, "code", alternate_window, std::nullopt, {})
                .has_value());
    CHECK(guile.active_workbench().value_or(WorkbenchId{}) == workbench);
    CHECK(guile.workbench_next(workbench).value_or(WorkbenchId{}) == other_workbench);
    CHECK(guile.workbench_next(other_workbench).value_or(WorkbenchId{}) == workbench);
    CHECK(guile.workbench_next(workbench, -1).value_or(WorkbenchId{}) == other_workbench);
    REQUIRE(guile.workbench_activate(other_workbench).has_value());
    CHECK(guile.active_workbench().value_or(WorkbenchId{}) == other_workbench);
    REQUIRE(guile.workbench_focus_window(other_workbench, alternate_window).has_value());
    CHECK(guile.workbench_active_window(other_workbench).value_or(WindowId{}) == alternate_window);
    CHECK_FALSE(guile.workbench_rename(other_workbench, "notes").value_or(true));
    REQUIRE(guile.workbench_activate(workbench).has_value());
    REQUIRE(guile.workbench_released(other_workbench).has_value());
    REQUIRE(guile.workbench_window_added(workbench, alternate_window).has_value());
    REQUIRE(guile.workbench_set_window_role(alternate_window, "tools").has_value());
    CHECK(guile.workbench_slot(workbench, "tools").value_or(std::optional<WindowId>{}) ==
          std::optional{alternate_window});
    REQUIRE(guile.workbench_set_window_role(window, "tools").has_value());
    CHECK(guile.workbench_slot(workbench, "tools").value_or(std::optional<WindowId>{}) ==
          std::optional{window});
    const std::expected<GuileWorkbenchWindowState, std::string> alternate_policy =
        guile.workbench_window_state(alternate_window);
    REQUIRE(alternate_policy.has_value());
    CHECK_FALSE(alternate_policy->window.role.has_value());
    REQUIRE(guile.workbench_set_window_pinned(window, true).has_value());
    REQUIRE(guile.workbench_set_window_created_by_policy(window, true).has_value());
    const GuileWorkbenchWindowState window_policy = guile.workbench_window_state(window).value();
    CHECK(window_policy.workbench == workbench);
    CHECK(window_policy.window.role == std::optional<std::string>{"tools"});
    CHECK(window_policy.window.pinned);
    CHECK(window_policy.window.created_by_policy);
    REQUIRE(guile.workbench_set_window_role(window, std::nullopt).has_value());
    CHECK(guile.workbench_slots(workbench).value_or(std::vector<GuileDisplaySlot>{}).empty());
    REQUIRE(guile.workbench_focus_window(workbench, window).has_value());
    CHECK(guile.workbench_forget_window(alternate_window).value_or(false));
    CHECK_FALSE(guile.workbench_forget_window(alternate_window).value_or(true));
    CHECK(guile.workbench_scope(workbench).value_or(std::vector<ProjectId>{}).empty());
    REQUIRE(guile.workbench_visit_buffer(workbench, visitor).has_value());
    REQUIRE(guile.workbench_visit_buffer(workbench, buffer).has_value());
    CHECK(guile.workbench_mru(workbench).value_or(std::vector<BufferId>{}) ==
          std::vector<BufferId>{buffer, visitor});
    REQUIRE(guile.replace_workbench_mru(workbench, {visitor, visitor, buffer}).has_value());
    CHECK(guile.workbench_mru(workbench).value_or(std::vector<BufferId>{}) ==
          std::vector<BufferId>{visitor, buffer});
    CHECK(guile.workbench_expel_buffer(workbench, visitor).value_or(false));
    const std::expected<GuileEvaluationResult, std::string> lsp_provider =
        guile.evaluate({.source = R"((use-modules (cind lsp))
(define-lsp-provider! host "clangd" "cpp" "clangd" '()
                      '("completion" "diagnostics" "navigation")))",
                        .source_name = "lsp-provider-test.scm"});
    REQUIRE(lsp_provider.has_value());
    REQUIRE_FALSE(lsp_provider->error.has_value());
    const std::expected<std::size_t, std::string> installed = guile.install_core_commands();
    REQUIRE(installed.has_value());
    CHECK(*installed == 233);
    const std::expected<std::size_t, std::string> providers = guile.install_core_providers();
    REQUIRE(providers.has_value());
    CHECK(*providers == 16);
    const auto feedback_message = [&] {
        const std::expected<GuileCommandFeedbackState, std::string> state =
            guile.command_feedback_state();
        REQUIRE(state.has_value());
        return state->message;
    };
    REQUIRE(guile.lsp_diagnostics_failed("invalid response").has_value());
    CHECK(feedback_message() == "LSP diagnostics failed: invalid response");
    const CommandId save = require_command(runtime, "file.save");
    runtime.buffers().set_resource(buffer, "/tmp/sample", BufferKind::File);

    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), cpp_mode);
    const CommandResult definition =
        runtime.commands().invoke(require_command(runtime, "lsp.definition"), context);
    REQUIRE(definition.has_value());
    REQUIRE(pending_async_request.has_value());
    const ScriptAsyncRequest navigation_request =
        pending_async_request.value_or(ScriptFileReadRequest{});
    const auto* navigation = std::get_if<ScriptLspNavigationRequest>(&navigation_request);
    REQUIRE(navigation != nullptr);
    CHECK(navigation->target.window == window);
    CHECK(navigation->target.buffer == buffer);
    CHECK(navigation->target.view == view);
    CHECK(navigation->kind == "definition");
    CHECK(navigation->session == 41);
    REQUIRE(ensured_lsp_target.has_value());
    CHECK(*ensured_lsp_target == CommandTarget{.window = window, .buffer = buffer, .view = view});
    REQUIRE(ensured_lsp_provider.has_value());
    CHECK(ensured_lsp_provider->name == "clangd");
    CHECK(ensured_lsp_provider->language_id == "cpp");
    CHECK(ensured_lsp_provider->command == "clangd");
    CHECK(ensured_lsp_provider->arguments.empty());
    CHECK(ensured_lsp_provider->root == "/tmp");
    CHECK(ensured_lsp_provider->features ==
          std::vector<std::string>{"completion", "diagnostics", "navigation"});
    CHECK(attached_lsp_sessions == std::vector<std::uint64_t>{41});
    REQUIRE(guile.lsp_session_bound(buffer, 41).value_or(false));
    REQUIRE(guile.buffer_edited(buffer, view, runtime.buffers().get(buffer).snapshot().revision())
                .has_value());
    CHECK(synchronized_lsp_sessions ==
          std::vector<std::pair<BufferId, std::uint64_t>>{{buffer, 41}});
    const std::uint64_t definition_task = next_async_task - 1;
    const CommandResult references =
        runtime.commands().invoke(require_command(runtime, "lsp.references"), context);
    REQUIRE(references.has_value());
    REQUIRE_FALSE(cancelled_async_tasks.empty());
    CHECK(cancelled_async_tasks.back() == definition_task);
    const ScriptAsyncRequest references_request =
        pending_async_request.value_or(ScriptFileReadRequest{});
    const auto* references_navigation =
        std::get_if<ScriptLspNavigationRequest>(&references_request);
    REQUIRE(references_navigation != nullptr);
    CHECK(references_navigation->kind == "references");
    CHECK(lsp_sessions_ensured == 2);
    CHECK(attached_lsp_sessions == std::vector<std::uint64_t>{41});
    pending_async_callbacks.completed(next_async_task - 1,
                                      ScriptLspNavigationResult{.locations = {}});
    CHECK(feedback_message() == "no LSP references found");
    REQUIRE(guile.buffer_released(buffer).has_value());
    CHECK_FALSE(guile.lsp_session_bound(buffer, 41).value_or(true));
    CHECK_FALSE(guile.buffer_style_origin(buffer).has_value());
    CHECK(guile.workbench_mru(workbench).value_or(std::vector<BufferId>{}).empty());
    runtime.buffers().get(buffer).modes().set_major(runtime.modes(), fundamental_mode);

    const std::uint32_t save_generation = runtime.buffers().get(buffer).save_generation();
    const CommandResult saved = runtime.commands().invoke(save, context);
    REQUIRE(saved.has_value());
    CHECK(guile.buffer_saving(buffer) == true);
    REQUIRE(pending_async_request.has_value());
    const auto* write = std::get_if<ScriptFileWriteRequest>(&*pending_async_request);
    REQUIRE(write != nullptr);
    CHECK(write->path == "/tmp/sample");
    CHECK(write->contents == "abc\n");
    CHECK(feedback_message() == "saving /tmp/sample…");
    pending_async_callbacks.completed(next_async_task - 1,
                                      ScriptFileWriteResult{.path = "/tmp/sample"});
    CHECK(runtime.buffers().get(buffer).save_generation() == save_generation + 1);
    CHECK_FALSE(runtime.buffers().get(buffer).modified());
    CHECK(guile.buffer_saving(buffer) == false);
    CHECK(feedback_message() == "saved /tmp/sample");

    const CommandResult session_saved = runtime.commands().invoke(
        require_command(runtime, "workbench.save-session.accept"), context,
        CommandInvocation{.arguments = {std::string("/tmp/cind-session")}, .prefix = {}});
    REQUIRE(session_saved.has_value());
    const auto* session_write = std::get_if<ScriptFileWriteRequest>(&*pending_async_request);
    REQUIRE(session_write != nullptr);
    CHECK(session_write->path == "/tmp/cind-session");
    CHECK(session_write->contents == workbench_session);
    CHECK(feedback_message() == "saving workbench session…");
    pending_async_callbacks.completed(next_async_task - 1,
                                      ScriptFileWriteResult{.path = "/tmp/cind-session"});
    CHECK(feedback_message() == "saved workbench session /tmp/cind-session");

    const CommandResult session_restoring = runtime.commands().invoke(
        require_command(runtime, "workbench.restore-session.accept"), context,
        CommandInvocation{.arguments = {std::string("/tmp/cind-session")}, .prefix = {}});
    REQUIRE(session_restoring.has_value());
    const auto* session_read = std::get_if<ScriptFileReadRequest>(&*pending_async_request);
    REQUIRE(session_read != nullptr);
    CHECK(session_read->path == "/tmp/cind-session");
    CHECK(feedback_message() == "reading workbench session…");
    pending_async_callbacks.completed(next_async_task - 1,
                                      ScriptFileReadResult{.path = "/tmp/cind-session",
                                                           .exists = true,
                                                           .contents = workbench_session});
    CHECK(restored_workbench_session == std::optional{workbench_session});
    CHECK(feedback_message() == "workbench session restored");

    REQUIRE(guile.restore_workbench_session("resource session").has_value());
    const std::uint64_t superseded_resource_task = next_async_task - 1;
    const auto* resource_read = std::get_if<ScriptFileReadRequest>(&*pending_async_request);
    REQUIRE(resource_read != nullptr);
    CHECK(resource_read->path == "/tmp/restored.cc");
    REQUIRE(guile.restore_workbench_session("replacement session").has_value());
    CHECK(std::ranges::find(cancelled_async_tasks, superseded_resource_task) !=
          cancelled_async_tasks.end());
    CHECK(restored_workbench_session == std::optional<std::string>{"replacement session"});
    CHECK(feedback_message() == "workbench session restored");

    REQUIRE(guile.workbench_visit_buffer(workbench, buffer).has_value());
    REQUIRE(guile.workbench_visit_buffer(workbench, other).has_value());
    CHECK(guile.workbench_buffers(workbench).value_or(std::vector<BufferId>{}) ==
          std::vector<BufferId>{other, buffer});

    const std::vector<InteractionCandidate> command_candidates =
        complete_provider(runtime, "commands", context);
    CHECK(std::ranges::any_of(command_candidates, [](const InteractionCandidate& candidate) {
        return candidate.value == "file.save" && candidate.detail == "command";
    }));
    CHECK(std::ranges::none_of(command_candidates, [](const InteractionCandidate& candidate) {
        return candidate.value.ends_with(".accept") || candidate.value.starts_with("interaction.");
    }));
    const std::vector<InteractionCandidate> filtered_commands =
        complete_provider(runtime, "commands", context, "file sav");
    REQUIRE_FALSE(filtered_commands.empty());
    CHECK(filtered_commands.front().value == "file.save");
    CHECK(std::ranges::all_of(filtered_commands, [](const InteractionCandidate& candidate) {
        return candidate.value.find("file") != std::string::npos &&
               candidate.value.find("sav") != std::string::npos;
    }));

    const std::vector<InteractionCandidate> buffer_candidates =
        complete_provider(runtime, "buffers", context);
    REQUIRE(buffer_candidates.size() == 2);
    CHECK(buffer_candidates[0].value == "other");
    CHECK(buffer_candidates[1].value == "sample");

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
    CHECK(help_mode == special_mode);
    CHECK(help_style_origin == "help");
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
    REQUIRE(guile.set_caret_reveal(false).has_value());
    const CommandResult buffer_search_result = runtime.commands().invoke(
        search_accept, context,
        CommandInvocation{.arguments = {true, std::string("bc")}, .prefix = {}});
    REQUIRE(buffer_search_result.has_value());
    CHECK(runtime.views().caret(view) == TextOffset{1});
    CHECK(feedback_message().empty());
    REQUIRE(guile.application_state().has_value());
    CHECK(guile.application_state()->reveal_caret);
    CHECK(runtime.commands().definition(search_accept).source == "scheme:(cind core)");

    REQUIRE(guile.set_message("").has_value());
    const CommandId search_next = require_command(runtime, "search.next");
    REQUIRE(runtime.commands().invoke(search_next, context).has_value());
    CHECK(runtime.views().caret(view) == TextOffset{1});
    CHECK(feedback_message() == "search wrapped");
    CHECK(runtime.commands().definition(search_next).source == "scheme:(cind core)");

    REQUIRE(guile.set_message("").has_value());
    const CommandId search_previous = require_command(runtime, "search.previous");
    REQUIRE(runtime.commands().invoke(search_previous, context).has_value());
    CHECK(runtime.views().caret(view) == TextOffset{1});
    CHECK(feedback_message() == "search wrapped");
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
          std::string("/tmp") + std::filesystem::path::preferred_separator);
    CHECK(open_request->provider == "files");
    CHECK(runtime.commands().definition(open_request->accept_command).name == "file.open.accept");

    const std::string directory = std::string("/tmp") + std::filesystem::path::preferred_separator;
    const std::size_t async_before_open = async_requests.size();
    const CommandResult directory_result =
        runtime.commands().invoke(open_request->accept_command, context,
                                  CommandInvocation{.arguments = {directory}, .prefix = {}});
    REQUIRE(directory_result.has_value());
    const auto* directory_request = std::get_if<InteractionRequest>(&*directory_result);
    REQUIRE(directory_request != nullptr);
    CHECK(directory_request->initial_input == directory);
    CHECK(async_requests.size() == async_before_open);

    const CommandResult relative_opened = runtime.commands().invoke(
        open_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("relative.cpp")}, .prefix = {}});
    REQUIRE(relative_opened.has_value());
    REQUIRE(async_requests.size() == async_before_open + 1);
    const auto* relative_opened_read =
        std::get_if<ScriptFileReadRequest>(&async_requests[async_before_open]);
    REQUIRE(relative_opened_read != nullptr);
    CHECK(relative_opened_read->path == "/tmp/relative.cpp");

    const CommandResult opened = runtime.commands().invoke(
        open_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("/tmp/example.cpp")}, .prefix = {}});
    REQUIRE(opened.has_value());
    REQUIRE(async_requests.size() == async_before_open + 2);
    const auto* opened_read =
        std::get_if<ScriptFileReadRequest>(&async_requests[async_before_open + 1]);
    REQUIRE(opened_read != nullptr);
    CHECK(opened_read->path == "/tmp/example.cpp");

    const CommandResult save_as_result =
        runtime.commands().invoke(require_command(runtime, "file.save-as"), context);
    REQUIRE(save_as_result.has_value());
    const auto* save_as_request = std::get_if<InteractionRequest>(&*save_as_result);
    REQUIRE(save_as_request != nullptr);
    CHECK(save_as_request->kind == InteractionKind::Text);
    CHECK(save_as_request->prompt == "Write file: ");
    CHECK(save_as_request->initial_input == "/tmp/sample");
    CHECK(runtime.commands().definition(save_as_request->accept_command).name ==
          "file.save-as.accept");
    const CommandResult save_as_accepted = runtime.commands().invoke(
        save_as_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("/tmp/written.cpp")}, .prefix = {}});
    REQUIRE(save_as_accepted.has_value());
    CHECK(runtime.buffers().get(buffer).resource_uri() == "/tmp/written.cpp");
    CHECK(runtime.buffers().get(buffer).name() == "written.cpp");
    CHECK(runtime.buffers().get(buffer).modes().major() == fundamental_mode);
    CHECK_FALSE(runtime.buffers().get(buffer).project_id().has_value());
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
    REQUIRE(buffer_released);
    CHECK(released_buffer == buffer);
    CHECK(replacement_buffer == other);
    CHECK_FALSE(created_buffer.has_value());

    buffer_released = false;
    const CommandResult force_killed =
        runtime.commands().invoke(require_command(runtime, "buffer.force-kill"), context);
    REQUIRE(force_killed.has_value());
    REQUIRE(buffer_released);
    CHECK(released_buffer == buffer);
    CHECK(replacement_buffer == other);

    EditTransaction modified = runtime.buffers().get(buffer).begin_transaction();
    modified.insert(TextOffset{}, "changed");
    (void)modified.commit();
    buffer_released = false;
    const CommandResult refused_kill =
        runtime.commands().invoke(require_command(runtime, "buffer.kill"), context);
    REQUIRE_FALSE(refused_kill.has_value());
    CHECK(refused_kill.error().message == "buffer has unsaved changes");
    CHECK_FALSE(buffer_released);

    const CommandResult saving = runtime.commands().invoke(save, context);
    REQUIRE(saving.has_value());
    const std::uint64_t saving_task = next_async_task - 1;
    const CommandResult saving_kill =
        runtime.commands().invoke(require_command(runtime, "buffer.force-kill"), context);
    REQUIRE_FALSE(saving_kill.has_value());
    CHECK(saving_kill.error().message == "buffer has a save in progress");
    pending_async_callbacks.cancelled(saving_task);
    CHECK(guile.buffer_saving(buffer) == false);

    release_error = "buffer release failed";
    const CommandResult failed_release =
        runtime.commands().invoke(require_command(runtime, "buffer.force-kill"), context);
    REQUIRE_FALSE(failed_release.has_value());
    CHECK(failed_release.error().message == "buffer release failed");
    release_error.clear();

    only_buffer = true;
    created_buffer.reset();
    buffer_released = false;
    const CommandResult last_buffer_killed =
        runtime.commands().invoke(require_command(runtime, "buffer.force-kill"), context);
    REQUIRE(last_buffer_killed.has_value());
    REQUIRE(created_buffer.has_value());
    CHECK(created_buffer->name == "*scratch*");
    CHECK(created_buffer->initial_text.empty());
    CHECK(created_buffer->kind == BufferKind::Scratch);
    CHECK_FALSE(created_buffer->read_only);
    CHECK(created_buffer->major_mode == fundamental_mode);
    CHECK(created_buffer->style_origin == "plain text");
    REQUIRE(buffer_released);
    CHECK(released_buffer == buffer);
    CHECK(replacement_buffer == other);
    only_buffer = false;

    Buffer& restored_buffer = runtime.buffers().get(buffer);
    EditTransaction restore = restored_buffer.begin_transaction();
    restore.replace(TextRange{TextOffset{}, restored_buffer.snapshot().content().end_offset()},
                    "abc\n");
    (void)restore.commit();
    restored_buffer.mark_saved(restored_buffer.snapshot().content());

    EditTransaction modify_for_quit = restored_buffer.begin_transaction();
    modify_for_quit.insert(restored_buffer.snapshot().content().end_offset(), "changed");
    (void)modify_for_quit.commit();
    const CommandResult quit_prompt =
        runtime.commands().invoke(require_command(runtime, "application.quit"), context);
    REQUIRE(quit_prompt.has_value());
    const auto* confirmation = std::get_if<InteractionRequest>(&*quit_prompt);
    REQUIRE(confirmation != nullptr);
    CHECK(confirmation->prompt == "1 modified buffer; exit anyway? (yes or no) ");
    CHECK(runtime.commands().definition(confirmation->accept_command).name ==
          "application.quit.accept");
    REQUIRE(guile.application_state().has_value());
    CHECK_FALSE(guile.application_state()->exit_requested);

    const CommandResult declined = runtime.commands().invoke(
        require_command(runtime, "application.quit.accept"), context,
        CommandInvocation{.arguments = {std::string("no")}, .prefix = {}});
    REQUIRE(declined.has_value());
    CHECK_FALSE(guile.application_state()->exit_requested);
    CHECK(feedback_message() == "quit cancelled");

    const CommandResult confirmed = runtime.commands().invoke(
        require_command(runtime, "application.quit.accept"), context,
        CommandInvocation{.arguments = {std::string("yes")}, .prefix = {}});
    REQUIRE(confirmed.has_value());
    CHECK(guile.application_state()->exit_requested);

    const CommandResult force_quit =
        runtime.commands().invoke(require_command(runtime, "application.force-quit"), context);
    REQUIRE(force_quit.has_value());
    REQUIRE(guile.application_state()->exit_requested);

    EditTransaction restore_after_quit = restored_buffer.begin_transaction();
    restore_after_quit.replace(
        TextRange{TextOffset{}, restored_buffer.snapshot().content().end_offset()}, "abc\n");
    (void)restore_after_quit.commit();
    restored_buffer.mark_saved(restored_buffer.snapshot().content());

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
    CHECK(feedback_message() == "other windows deleted");

    const CommandResult other_window =
        runtime.commands().invoke(require_command(runtime, "window.other"), context);
    REQUIRE(other_window.has_value());
    REQUIRE(other_window_selected);
    CHECK(focused_window == alternate_window);

    REQUIRE(guile.set_caret_reveal(false).has_value());
    const CommandResult redraw =
        runtime.commands().invoke(require_command(runtime, "editor.redraw"), context);
    REQUIRE(redraw.has_value());
    CHECK(guile.application_state()->reveal_caret);

    REQUIRE(guile.set_caret_reveal(false).has_value());
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
    CHECK(guile.application_state()->reveal_caret);

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
    CHECK(feedback_message() == "C-x C-s  file.save");

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
    CHECK(project_candidates[0].value == "/tmp/sample/src/main.cpp");
    CHECK(project_candidates[0].label == "src/main.cpp");
    CHECK(project_candidates[0].detail == "src");

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
    CHECK_FALSE(project_index_requested);

    const ProjectId unindexed_project = runtime.projects().create({.name = "unindexed",
                                                                   .roots = {"/tmp/unindexed"},
                                                                   .discovery_provider = {},
                                                                   .discovery_marker = {}});
    runtime.projects().assign(buffer, unindexed_project);
    const CommandResult unindexed_find_file = runtime.commands().invoke(project_find_file, context);
    REQUIRE(unindexed_find_file.has_value());
    REQUIRE(project_index_requested);
    CHECK(indexed_project == unindexed_project);
    runtime.projects().assign(buffer, project);
    project_index_requested = false;
    CHECK(guile.workbench_adopt_project(workbench, project).value_or(false));
    CHECK_FALSE(guile.workbench_adopt_project(workbench, project).value_or(true));
    CHECK(guile.workbench_scope(workbench).value_or(std::vector<ProjectId>{}) ==
          std::vector<ProjectId>{project});

    const std::size_t async_before_project_file = async_requests.size();
    const CommandResult file_accepted = runtime.commands().invoke(
        find_file_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("/tmp/sample/main.cpp")}, .prefix = {}});
    REQUIRE(file_accepted.has_value());
    REQUIRE(async_requests.size() == async_before_project_file + 1);
    const auto* project_file_read =
        std::get_if<ScriptFileReadRequest>(&async_requests[async_before_project_file]);
    REQUIRE(project_file_read != nullptr);
    CHECK(project_file_read->path == "/tmp/sample/main.cpp");

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
    const std::size_t async_before_project_search = async_requests.size();
    const CommandResult search_accepted = runtime.commands().invoke(
        search_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("needle")}, .prefix = {}});
    REQUIRE(search_accepted.has_value());
    REQUIRE(async_requests.size() == async_before_project_search + 1);
    const auto* search_process =
        std::get_if<ScriptProcessRequest>(&async_requests[async_before_project_search]);
    REQUIRE(search_process != nullptr);
    CHECK(search_process->file == "rg");
    CHECK(search_process->working_directory == "/tmp/sample");
    CHECK(search_process->arguments ==
          std::vector<std::string>{"--line-number", "--column", "--no-heading", "--color", "never",
                                   "--smart-case", "--null", "--", "needle", "."});
    CHECK(guile.project_search_running());

    const std::uint64_t search_process_task = next_async_task - 1;
    const std::string search_output = std::string("src/main.cpp\0", 13) + "1:1:needle\n";
    pending_async_callbacks.completed(search_process_task,
                                      ScriptProcessResult{.exit_status = 0,
                                                          .term_signal = 0,
                                                          .standard_output = search_output,
                                                          .standard_error = {}});
    REQUIRE(async_requests.size() == async_before_project_search + 2);
    const auto* search_parse = std::get_if<ScriptRgResultParseRequest>(&async_requests.back());
    REQUIRE(search_parse != nullptr);
    CHECK(search_parse->project_root == "/tmp/sample");
    CHECK(search_parse->output == search_output);

    const std::uint64_t search_parse_task = next_async_task - 1;
    const CommandResult replacement_search = runtime.commands().invoke(
        search_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("replacement")}, .prefix = {}});
    REQUIRE(replacement_search.has_value());
    REQUIRE_FALSE(cancelled_async_tasks.empty());
    CHECK(cancelled_async_tasks.back() == search_parse_task);
    const auto* replacement_process = std::get_if<ScriptProcessRequest>(&async_requests.back());
    REQUIRE(replacement_process != nullptr);
    CHECK(replacement_process->arguments[8] == "replacement");
    CHECK(guile.project_search_running());

    const std::uint64_t replacement_process_task = next_async_task - 1;
    pending_async_callbacks.completed(replacement_process_task,
                                      ScriptProcessResult{.exit_status = 2,
                                                          .term_signal = 0,
                                                          .standard_output = {},
                                                          .standard_error = "rg failed\n"});
    CHECK_FALSE(guile.project_search_running());
    CHECK(feedback_message() == "project search failed: rg failed");

    const ProjectId tools_project = runtime.projects().create({.name = "tools",
                                                               .roots = {"/tmp/tools"},
                                                               .discovery_provider = {},
                                                               .discovery_marker = {}});
    runtime.projects().replace_index(tools_project, {"/tmp/tools/src/tool.cpp"});
    CHECK(guile.workbench_adopt_project(workbench, tools_project).value_or(false));
    CHECK(guile.workbench_scope(workbench).value_or(std::vector<ProjectId>{}) ==
          std::vector<ProjectId>{project, tools_project});
    const std::vector<InteractionCandidate> scoped_candidates =
        complete_provider(runtime, "project-files", context);
    REQUIRE(scoped_candidates.size() == 3);
    CHECK(std::ranges::any_of(scoped_candidates, [](const InteractionCandidate& candidate) {
        return candidate.value == "/tmp/sample/src/main.cpp" && candidate.detail == "/tmp/sample";
    }));
    CHECK(std::ranges::any_of(scoped_candidates, [](const InteractionCandidate& candidate) {
        return candidate.value == "/tmp/tools/src/tool.cpp" && candidate.detail == "/tmp/tools";
    }));

    const CommandResult scoped_search = runtime.commands().invoke(project_search, context);
    REQUIRE(scoped_search.has_value());
    const auto* scoped_search_request = std::get_if<InteractionRequest>(&*scoped_search);
    REQUIRE(scoped_search_request != nullptr);
    const std::size_t async_before_scoped_search = async_requests.size();
    const CommandResult scoped_search_accepted = runtime.commands().invoke(
        scoped_search_request->accept_command, context,
        CommandInvocation{.arguments = {std::string("shared")}, .prefix = {}});
    REQUIRE(scoped_search_accepted.has_value());
    REQUIRE(async_requests.size() == async_before_scoped_search + 1);
    const auto* scoped_search_process =
        std::get_if<ScriptProcessRequest>(&async_requests[async_before_scoped_search]);
    REQUIRE(scoped_search_process != nullptr);
    CHECK(scoped_search_process->working_directory == "/tmp/sample");
    CHECK(scoped_search_process->arguments ==
          std::vector<std::string>{"--line-number", "--column", "--no-heading", "--color", "never",
                                   "--smart-case", "--null", "--", "shared", "/tmp/sample",
                                   "/tmp/tools"});
    pending_async_callbacks.failed(next_async_task - 1, "stopped");
    CHECK_FALSE(guile.project_search_running());
    runtime.views().set_caret(view, TextOffset{0});
    CommandLoop command_loop(runtime);
    const CommandId toggle_mark = require_command(runtime, "selection.toggle-mark");
    CHECK(command_loop.execute(toggle_mark, context).status == CommandLoopStatus::Executed);
    CHECK(runtime.views().mark(view) == std::optional<TextOffset>{TextOffset{0}});
    CHECK(feedback_message() == "mark set");
    CHECK(command_loop.execute(toggle_mark, context).status == CommandLoopStatus::Executed);
    CHECK_FALSE(runtime.views().mark(view).has_value());
    CHECK(feedback_message() == "mark cleared");

    runtime.views().set_selection(view, {.anchor = TextOffset{0}, .head = TextOffset{1}});
    command_loop.set_pending_prefix({.count = std::nullopt, .register_name = "a", .extra = {}});
    CHECK(command_loop.execute(require_command(runtime, "edit.copy-region"), context).status ==
          CommandLoopStatus::Executed);
    CHECK(clipboard == "a");
    CHECK(feedback_message() == "copied");
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
    CHECK_FALSE(requested_structural_kill);
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
    CHECK(snapshot.scripted_commands == 233);
    CHECK(snapshot.provider_revision == 1);
    CHECK(snapshot.scripted_providers == 16);
    CHECK_FALSE(snapshot.last_error.has_value());
}
