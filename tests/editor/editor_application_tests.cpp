#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/editor_application.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
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

class TemporaryFile {
public:
    TemporaryFile(std::string name, std::string_view contents)
        : path_(std::filesystem::temp_directory_path() / std::move(name)) {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output) {
            throw std::runtime_error("cannot create temporary test file");
        }
    }

    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string name)
        : path_(std::filesystem::temp_directory_path() / std::move(name)) {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        if (!std::filesystem::create_directories(path_)) {
            throw std::runtime_error("cannot create temporary test directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    std::filesystem::path write(std::string name, std::string_view contents) const {
        const std::filesystem::path path = path_ / std::move(name);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output) {
            throw std::runtime_error("cannot write temporary test file");
        }
        return path;
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class ScopedPath {
public:
    explicit ScopedPath(const std::filesystem::path& directory)
        : previous_(std::getenv("PATH") != nullptr ? std::getenv("PATH") : "") {
        const std::string value = directory.string() + ":" + previous_;
        if (::setenv("PATH", value.c_str(), 1) != 0) {
            throw std::runtime_error("cannot set test PATH");
        }
    }

    ~ScopedPath() { (void)::setenv("PATH", previous_.c_str(), 1); }

    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;

private:
    std::string previous_;
};

EditorApplication make_application(std::string path, std::string initial,
                                   EditorPlatformServices platform_services = {}) {
    return EditorApplication({.path = std::move(path),
                              .initial_text = std::move(initial),
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

GuileInteractionPolicyState interaction_policy(const EditorApplication& application) {
    const std::expected<std::optional<GuileInteractionPolicyState>, std::string> state =
        application.interaction_policy_state();
    if (!state) {
        throw std::runtime_error(state.error());
    }
    if (!*state) {
        throw std::runtime_error("no interaction policy state is active");
    }
    return **state;
}

CommandId require_command(const EditorRuntime& runtime, std::string_view name) {
    const std::optional<CommandId> command = runtime.commands().find(name);
    if (!command) {
        FAIL("missing command: ", name);
        return {};
    }
    return *command;
}

class RecordingStructuralSession final : public LanguageMechanismSession {
public:
    explicit RecordingStructuralSession(std::shared_ptr<int> calls) : calls_(std::move(calls)) {}

    TypeCharsResult type_chars(Document& document, std::span<const TextOffset> carets,
                               char character, const CppIndentStyle&) override {
        ++*calls_;
        EditTransaction transaction = document.begin_transaction();
        const std::string replacement(
            1, static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
        for (auto caret = carets.rbegin(); caret != carets.rend(); ++caret) {
            transaction.insert(*caret, replacement);
        }
        CommitResult commit = transaction.commit();
        return {.carets = {}, .decisions = {}, .change = std::move(commit.change)};
    }

private:
    std::shared_ptr<int> calls_;
};

} // namespace

TEST_CASE("editor application owns normalized interaction dispatch") {
    EditorApplication application = make_application("sample.cc", "one two one");

    send_keys(application, "C-s");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "search: ");
    // The interaction mechanism creates its buffer without announcing it, so
    // it has no name in the registry that owns names; the interaction carries
    // its own. Folding interaction buffers into that registry is the next P2
    // slice (design/09-guile-first.md section 8).
    CHECK(interaction_policy(application).buffer_name == " *minibuffer*");
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

TEST_CASE("scripted language profiles dispatch the selected executable mechanism") {
    EditorApplication application = make_application("sample.cc", "");
    EditorRuntime& runtime = application.runtime();
    const auto calls = std::make_shared<int>(0);
    const auto mechanism = std::make_shared<LanguageMechanism>(
        language_facet_bit(LanguageFacet::StructuralEditing),
        [calls] { return std::make_unique<RecordingStructuralSession>(calls); });
    const LanguageProviderId provider = runtime.languages().define_provider(
        "test.uppercase-input", LanguageFacet::StructuralEditing, mechanism);
    const LanguageProfileId profile = runtime.languages().define_profile("test.uppercase");
    runtime.languages().bind(profile, LanguageFacet::StructuralEditing, provider);
    const ModeId mode = runtime.modes().define("test-uppercase-mode", ModeKind::Major, profile);
    runtime.modes().set_interaction_class(mode, InteractionClass::Editing);
    application.session().buffer().modes().set_major(runtime.modes(), mode);

    application.insert_text("x");

    CHECK(application.session().snapshot().content() == "X");
    CHECK(*calls == 1);
}

TEST_CASE("query replace is a shared scripted command") {
    EditorApplication application = make_application("sample.cc", "text");

    send_keys(application, "M-%");
    CHECK(application.last_command() == "search.replace");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "Replace: ");
    CHECK(application.runtime()
              .commands()
              .definition(require_command(application.runtime(), "search.replace"))
              .source == "scheme:(cind core)");
}

TEST_CASE("basic editing policy is owned by Guile") {
    EditorApplication application = make_application("sample.cc", "text");
    constexpr std::array<std::string_view, 17> commands{"edit.self-insert",
                                                        "edit.undo",
                                                        "edit.redo",
                                                        "cursor.line-start",
                                                        "cursor.line-end",
                                                        "cursor.next-line",
                                                        "cursor.previous-line",
                                                        "cursor.page-down",
                                                        "cursor.page-up",
                                                        "cursor.forward-character",
                                                        "cursor.backward-character",
                                                        "edit.delete-backward",
                                                        "edit.delete-forward",
                                                        "edit.delete-backward-raw",
                                                        "edit.delete-forward-raw",
                                                        "edit.newline",
                                                        "edit.indent"};
    for (const std::string_view name : commands) {
        const CommandRegistry::Definition& definition = application.runtime().commands().definition(
            require_command(application.runtime(), name));
        CHECK(definition.source == "scheme:(cind core)");
    }
    CHECK(application.input_state().text_command == std::optional<std::string>{"edit.self-insert"});
}

TEST_CASE("interaction and position command policy is owned by Guile") {
    EditorApplication application = make_application("sample.cc", "abc\n");
    constexpr std::array<std::string_view, 7> commands{"keyboard.quit",
                                                       "interaction.submit",
                                                       "interaction.next-candidate",
                                                       "interaction.previous-candidate",
                                                       "interaction.previous-history",
                                                       "interaction.next-history",
                                                       "editor.position"};
    for (const std::string_view name : commands) {
        const CommandRegistry::Definition& definition = application.runtime().commands().definition(
            require_command(application.runtime(), name));
        CHECK(definition.source == "scheme:(cind core)");
    }

    application.session().set_caret(TextOffset{2});
    send_keys(application, "C-x =");
    CHECK(application.last_command() == "editor.position");
    CHECK(application.message() == "line 1/2, column 3, byte 2/4");
}

TEST_CASE("location-list command policy is owned by Guile") {
    EditorApplication application = make_application("sample.cc", "abc\n");
    constexpr std::array<std::string_view, 5> commands{"location.visit", "location.next",
                                                       "location.previous", "location.next-error",
                                                       "location.previous-error"};
    for (const std::string_view name : commands) {
        const CommandRegistry::Definition& definition = application.runtime().commands().definition(
            require_command(application.runtime(), name));
        CHECK(definition.source == "scheme:(cind core)");
    }
}

TEST_CASE("query replace composes minibuffer and single-key input policy") {
    EditorApplication application = make_application("sample.cc", "é two é");

    send_keys(application, "M-%");
    application.insert_text("é");
    send_keys(application, "RET");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "Replace é with: ");

    application.insert_text("e");
    send_keys(application, "RET");
    CHECK_FALSE(application.interaction().active());
    REQUIRE(application.session().active_selection().has_value());
    const ViewSelection first_match = *application.session().active_selection();
    CHECK(first_match.ranges[first_match.primary].ordered() ==
          TextRange{TextOffset{0}, TextOffset{2}});

    send_keys(application, "y");
    CHECK(application.session().snapshot().content() == "e two é");
    REQUIRE(application.session().active_selection().has_value());
    const ViewSelection second_match = *application.session().active_selection();
    CHECK(second_match.ranges[second_match.primary].ordered() ==
          TextRange{TextOffset{6}, TextOffset{8}});

    send_keys(application, "!");
    CHECK(application.session().snapshot().content() == "e two e");
    CHECK_FALSE(application.session().active_selection().has_value());
    CHECK(application.message() == "replaced 2 occurrences");
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
    const std::optional<LanguageProfileId> cpp_language =
        application.runtime().languages().find_profile("cind.cpp");
    REQUIRE(cpp_language.has_value());
    CHECK(modes.definition(cpp).language == cpp_language);
    CHECK(modes.definition(locations).parent == special);
    CHECK(modes.effective_policy(application.session().buffer().modes()).interaction_class ==
          InteractionClass::Editing);
}

TEST_CASE("resource policy selects file modes without making scratch buffers C++") {
    EditorApplication cpp = make_application("sample.cpp", "int value;\n");
    EditorApplication scheme = make_application("sample.scm", "(define value 1)\n");
    EditorApplication text = make_application("README.md", "text\n");
    EditorApplication scratch = make_application({}, "text\n");

    const auto major_name = [](const EditorApplication& application) {
        const ModeId mode = application.session().buffer().modes().major().value_or(ModeId{});
        return application.runtime().modes().definition(mode).name;
    };
    CHECK(major_name(cpp) == "cind.cpp");
    CHECK(major_name(scheme) == "scheme-mode");
    CHECK(major_name(text) == "fundamental-mode");
    CHECK(major_name(scratch) == "fundamental-mode");
    CHECK_FALSE(scheme.syntax_tokens().empty());

    send_keys(scheme, "C-c C-e");
    REQUIRE(scheme.interaction().state() != nullptr);
    CHECK(interaction_policy(scheme).prompt == "Eval: ");

    EditorApplication plain_scheme = make_application("plain.scm", "{");
    plain_scheme.session().set_caret(TextOffset{1});
    send_keys(plain_scheme, "RET");
    CHECK(plain_scheme.session().snapshot().content().to_string() == "{\n ");
    CHECK(plain_scheme.session().caret() == TextOffset{3});
    CHECK(plain_scheme.message().empty());
    plain_scheme.insert_text("\"");
    send_keys(plain_scheme, "Backspace");
    CHECK(plain_scheme.session().snapshot().content().to_string() == "{\n ");
    send_keys(plain_scheme, "TAB");
    CHECK(plain_scheme.message() == "indent: ParenContinuation");
}

TEST_CASE("Scheme mode navigates datums and enables structural editing") {
    const std::string source = "; ignored ( delimiter\n'(define (square x) \"not )\")\n#u8(1 2)\n";
    EditorApplication application = make_application("sample.scm", source);
    const std::size_t quoted_start = source.find('\'');
    const std::size_t quoted_end = source.find("\n#u8");
    const std::size_t vector_start = source.find("#u8");
    const std::size_t vector_end = source.find('\n', vector_start);
    const std::size_t nested_start = source.find("(square");

    application.session().set_caret(TextOffset{0});
    send_keys(application, "C-M-f");
    CHECK(application.session().caret().value == quoted_end);
    send_keys(application, "C-M-f");
    CHECK(application.session().caret().value == vector_end);
    send_keys(application, "C-M-b");
    CHECK(application.session().caret().value == vector_start);
    send_keys(application, "C-M-b");
    CHECK(application.session().caret().value == quoted_start);

    application.session().set_caret(TextOffset{static_cast<std::uint32_t>(nested_start + 4)});
    send_keys(application, "C-M-u");
    CHECK(application.session().caret().value == nested_start);
    CHECK(application.session().has_language_facet(LanguageFacet::StructuralEditing));
    const ModeId paredit = application.runtime().modes().find("paredit-mode").value_or(ModeId{});
    REQUIRE(paredit);
    CHECK(std::ranges::find(application.session().buffer().modes().minors(), paredit) ==
          application.session().buffer().modes().minors().end());
}

TEST_CASE("Scheme typing pairs delimiters and paredit transforms lists") {
    EditorApplication application = make_application("sample.scm", "");
    application.insert_text("(");
    CHECK(application.session().snapshot().content() == "()");
    CHECK(application.session().caret() == TextOffset{1});
    application.insert_text("define value 1");
    application.insert_text(")");
    CHECK(application.session().snapshot().content() == "(define value 1)");

    EditorApplication transforms = make_application("sample.scm", "(one (two three) four)");
    const ModeId paredit = transforms.runtime().modes().find("paredit-mode").value_or(ModeId{});
    REQUIRE(paredit);
    REQUIRE(transforms.execute_command("paredit.mode"));
    CHECK(std::ranges::find(transforms.session().buffer().modes().minors(), paredit) !=
          transforms.session().buffer().modes().minors().end());
    transforms.session().set_caret(TextOffset{10});
    send_keys(transforms, "C-)");
    CHECK(transforms.session().snapshot().content() == "(one (two three four))");
    send_keys(transforms, "M-s");
    CHECK(transforms.session().snapshot().content() == "(one two three four)");

    EditorApplication forward_barf = make_application("sample.scm", "(one (two three four))");
    forward_barf.session().set_caret(TextOffset{11});
    REQUIRE(forward_barf.session().edit_structure(StructuralEdit::ForwardBarf));
    CHECK(forward_barf.session().snapshot().content() == "(one (two three) four)");

    EditorApplication backward_slurp = make_application("sample.scm", "zero (one two)");
    backward_slurp.session().set_caret(TextOffset{9});
    REQUIRE(backward_slurp.session().edit_structure(StructuralEdit::BackwardSlurp));
    CHECK(backward_slurp.session().snapshot().content() == "(zero one two)");

    EditorApplication backward_barf = make_application("sample.scm", "(zero one two)");
    backward_barf.session().set_caret(TextOffset{8});
    REQUIRE(backward_barf.session().edit_structure(StructuralEdit::BackwardBarf));
    CHECK(backward_barf.session().snapshot().content() == "zero (one two)");
}

TEST_CASE("user initialization overrides file mode policy before the first buffer") {
    TemporaryFile init(std::format("cind-mode-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((%define-mode! host 'user-notes 'major 'fundamental-mode #f #f
                 'editing #f '() #f)
(define-file-mode-rule! host 'user-notes-rule 'user-notes '(".notes") '())
)");
    EditorApplication application({.path = "sample.notes",
                                   .initial_text = "notes\n",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});

    const ModeId major = application.session().buffer().modes().major().value_or(ModeId{});
    CHECK(application.runtime().modes().definition(major).name == "user-notes");
    CHECK(application.scripting().mode_revision == 2);
    CHECK(application.scripting().resource_policy_revision == 2);
    CHECK(application.scripting().scripted_file_mode_rules == 3);
}

TEST_CASE("user initialization owns root keymap policy") {
    TemporaryFile init(std::format("cind-keymap-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((define-keymap! host 'user.editor #f)
(define-keymap! host 'user.application #f)
(define-keymap! host 'user.override #f)
(configure-keymap-policy! host
                          #:editor '(user.editor)
                          #:application '(user.application)
                          #:overrides '(user.override))
)");
    EditorApplication application({.path = "sample.cc",
                                   .initial_text = "text",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});
    const KeymapRegistry& keymaps = application.runtime().keymaps();
    const std::optional<KeymapId> editor = keymaps.find("user.editor");
    const std::optional<KeymapId> global = keymaps.find("user.application");
    const std::optional<KeymapId> override = keymaps.find("user.override");
    REQUIRE(editor);
    REQUIRE(global);
    REQUIRE(override);

    const std::span<const KeymapLayer> layers = application.active_keymap_layers();
    REQUIRE(layers.size() >= 2);
    CHECK(layers[layers.size() - 2].keymap == *editor);
    CHECK(layers[layers.size() - 2].scope == "editor");
    CHECK(layers.back().keymap == *global);
    CHECK(layers.back().scope == "global");
    REQUIRE(application.command_loop().override_keymaps().size() == 1);
    CHECK(application.command_loop().override_keymaps().front() == *override);
    CHECK(application.chrome_content().echo.empty());
}

TEST_CASE("user completion policy is projected into native sessions") {
    TemporaryFile init(std::format("cind-completion-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-completion-policy!
 host
 (lambda (host context trigger)
   #(completion-policy #(kind match-tier label) #t 3)))
)");
    EditorApplication application({.path = "sample.scm",
                                   .initial_text = "(def",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});
    application.session().set_caret(TextOffset{4});

    send_keys(application, "C-M-i");
    const CompletionState* state = application.completion().state();
    REQUIRE(state != nullptr);
    CHECK(state->policy.rank_keys == std::vector{CompletionRankKey::Kind,
                                                 CompletionRankKey::MatchTier,
                                                 CompletionRankKey::Label});
    CHECK(state->policy.visible_resolve_count == 3);
}

TEST_CASE("shared echo policy follows active bindings and input lifetime") {
    EditorApplication application = make_application("sample.cc", "text");
    const std::string idle = application.chrome_content().echo;
    CHECK(idle.find("save") != std::string::npos);
    CHECK(idle.find("commands") != std::string::npos);

    application.set_message("transient message");
    CHECK(application.chrome_content().echo == "transient message");
    send_keys(application, "C-f");
    CHECK(application.message().empty());
    CHECK(application.chrome_content().echo == idle);
}

TEST_CASE("user initialization owns modeline content policy") {
    TemporaryFile init(std::format("cind-modeline-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-modeline-policy!
 host
 (lambda (host context facts)
   (vector 'modeline
           (vector (vector 'modeline-segment 'left 'salient 'strong #f
                           (string-append "custom:"
                                          (buffer-name host (context-buffer context))))))))
)");
    EditorApplication application({.path = "sample.cc",
                                   .initial_text = "text",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});
    const ModelineContent content = application.modeline(application.window_id());
    REQUIRE(content.segments.size() == 1);
    CHECK(content.segments.front().text == "custom:sample.cc");
    CHECK(content.segments.front().group == ModelineGroup::Left);
    CHECK(content.segments.front().tone == ModelineTone::Salient);
    CHECK(content.segments.front().weight == ModelineWeight::Strong);
}

TEST_CASE("command feedback exposes the last key only in the active modeline") {
    EditorApplication application = make_application("sample.cc", "text");
    REQUIRE(application.split_window(WindowSplitAxis::Columns));
    send_keys(application, "C-f");

    const std::vector<OpenWindowSnapshot> windows = application.open_windows();
    REQUIRE(windows.size() == 2);
    for (const OpenWindowSnapshot& window : windows) {
        const ModelineContent content = application.modeline(window.window);
        const bool contains_last_key = std::ranges::any_of(
            content.segments, [](const ModelineSegment& segment) { return segment.text == "C-f"; });
        CHECK(contains_last_key == window.active);
    }
}

TEST_CASE("user initialization owns editor chrome policy") {
    TemporaryFile init(std::format("cind-chrome-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-chrome-policy!
 host
 (lambda (host context facts)
   (vector 'chrome "custom-key" "custom-echo" 6 "custom-popup"
           (vector (vector 'chrome-item "first" "detail"))
           1 0 "query" 2)))
)");
    EditorApplication application({.path = "sample.cc",
                                   .initial_text = "text",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});
    const ChromeContent content = application.chrome_content();
    CHECK(content.pending_key == "custom-key");
    CHECK(content.echo == "custom-echo");
    CHECK(content.echo_cursor_byte == 6);
    CHECK(content.echo_cursor_column == 6);
    CHECK(content.popup_title == "custom-popup");
    REQUIRE(content.popup_items.size() == 1);
    CHECK(content.popup_items.front() ==
          ChromeItem{.label = "first", .detail = "detail", .kind = {}});
    CHECK(content.popup_capacity == 1);
    CHECK(content.popup_selection == 0);
    CHECK(content.popup_input == "query");
    CHECK(content.popup_input_cursor == 2);
}

TEST_CASE("user initialization owns frontend presentation policy") {
    TemporaryFile init(std::format("cind-theme-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-theme-policy!
 host
 (lambda (host)
   (vector 'presentation-theme
           #xff000001 #xff000002 #xff000003 #xff000004
           #xff000005 #xff000006 #xff000007 #xff000008
           #xff000009 #xff00000a #xff00000b #xff00000c
           #xff00000d #xff00000e #xff00000f #xff000010)))
(define custom-styles (resolve-presentation-styles host))
(vector-set! (vector-ref (vector-ref custom-styles 3) 1) 2 #xff00f00d)
(configure-style-policy!
 host
 (lambda (host theme) custom-styles))
(configure-motion-policy!
 host
 (lambda (host)
   (vector 'presentation-motion 90 24.0 0.002 0.02)))
(configure-metrics-policy!
 host
 (lambda (host)
   (vector 'presentation-metrics
           13.0 9.0 11.0 7.0 9.0 15.0 12.0 3.0 42 7)))
(configure-typography-policy!
 host
 (lambda (host)
   (vector 'presentation-typography "Test Mono" 18.5)))
)");
    EditorApplication application({.path = "sample.cc",
                                   .initial_text = "text",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});

    const PresentationTheme theme = application.presentation_theme();
    CHECK(theme.canvas == 0xFF000001);
    CHECK(theme.salient == 0xFF00000A);
    CHECK(theme.sign_deleted == 0xFF000010);
    const PresentationStyleSheet styles = application.presentation_styles();
    CHECK(styles.style(PresentationTextRole::Text).foreground == 0xFF000006);
    CHECK(styles.style(PresentationTextRole::Keyword).foreground == 0xFF00F00D);
    CHECK(styles.style(PresentationTextRole::Preprocessor).foreground == 0xFF00000C);
    CHECK(styles.style(PresentationTextRole::StatusBar).background == 0xFF000003);
    CHECK(styles.style(PresentationTextRole::StatusKey).weight == PresentationWeight::Strong);
    CHECK(styles.modeline[static_cast<std::size_t>(ModelineTone::Critical)] == 0xFF00000C);
    CHECK(styles.inactive_alpha == 0xB0);
    const PresentationMotion motion = application.presentation_motion();
    CHECK(motion.view_duration_ms == 90);
    CHECK(motion.scroll_spring_frequency == doctest::Approx(24.0F));
    CHECK(motion.scroll_position_tolerance == doctest::Approx(0.002F));
    CHECK(motion.scroll_velocity_tolerance == doctest::Approx(0.02F));
    const PresentationMetrics metrics = application.presentation_metrics();
    CHECK(metrics.modeline_extra_height == doctest::Approx(13.0F));
    CHECK(metrics.echo_extra_height == doctest::Approx(9.0F));
    CHECK(metrics.footer_padding_x == doctest::Approx(11.0F));
    CHECK(metrics.segment_gap == doctest::Approx(7.0F));
    CHECK(metrics.chip_padding_x == doctest::Approx(9.0F));
    CHECK(metrics.minibuffer_padding_x == doctest::Approx(15.0F));
    CHECK(metrics.minibuffer_detail_gap == doctest::Approx(12.0F));
    CHECK(metrics.cursor_stroke == doctest::Approx(3.0F));
    CHECK(metrics.minimum_columns == 42);
    CHECK(metrics.minimum_rows == 7);
    const PresentationTypography typography = application.presentation_typography();
    CHECK(typography.font_family == "Test Mono");
    CHECK(typography.font_size == doctest::Approx(18.5F));
}

TEST_CASE("frontend acquires presentation policy as one coherent profile") {
    TemporaryFile init(
        std::format("cind-presentation-profile-{}.scm", static_cast<long>(::getpid())),
        R"((define stable-styles
  (vector-ref (resolve-presentation-profile host) 2))
(define theme-calls 0)
(configure-theme-policy!
 host
 (lambda (host)
   (set! theme-calls (+ theme-calls 1))
   (vector 'presentation-theme
           #xff100001 #xff100002 #xff100003 #xff100004
           #xff100005 #xff100006 #xff100007 #xff100008
           #xff100009 #xff10000a #xff10000b #xff10000c
           #xff10000d #xff10000e #xff10000f #xff100010)))
(configure-style-policy!
 host
 (lambda (host theme)
   (unless (= theme-calls 1)
     (error "theme and styles were not resolved atomically" theme-calls))
   stable-styles))
)");
    EditorApplication application({.path = "sample.cc",
                                   .initial_text = "text",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});

    CHECK(application.presentation_theme().canvas == 0xFF100001);
}

TEST_CASE("user initialization owns startup buffer policy before native bootstrap") {
    TemporaryFile init(std::format("cind-startup-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-startup-policy!
 host
 (lambda (host facts)
   (vector 'startup-plan
           (vector 'startup-buffer "*Welcome*" 'empty 'generated #f #t 'special-mode)
           #f "custom startup" #f #f)))
)");
    EditorApplication application({.path = "ignored.cpp",
                                   .initial_text = "ignored contents",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});

    const Buffer& buffer = application.session().buffer();
    CHECK(application.buffer_name(buffer.id()) == "*Welcome*");
    CHECK(application.buffer_kind(buffer.id()) == BufferKind::Generated);
    CHECK(buffer.read_only());
    CHECK(buffer.snapshot().content().empty());
    const ModeId mode = buffer.modes().major().value_or(ModeId{});
    REQUIRE(mode);
    CHECK(application.runtime().modes().definition(mode).name == "special-mode");
    CHECK(application.style_origin() == "custom startup");
    CHECK_FALSE(application.has_background_work());
}

TEST_CASE("user initialization owns the last-buffer fallback policy") {
    TemporaryFile init(std::format("cind-fallback-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-fallback-buffer-policy!
 host
 (lambda (host)
   (create-buffer! host "*Fallback*" "ready\n" 'scratch #f #f
                   'fundamental-mode #f "custom fallback")))
)");
    EditorApplication application({.path = {},
                                   .initial_text = "source",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});

    REQUIRE(application.execute_command("buffer.force-kill"));
    CHECK(application.buffer_count() == 1);
    CHECK(application.buffer_name(application.session().buffer().id()) == "*Fallback*");
    CHECK(application.session().snapshot().content() == "ready\n");
    CHECK(application.style_origin() == "custom fallback");
}

TEST_CASE("user initialization owns close request command selection") {
    TemporaryFile init(std::format("cind-close-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-close-policy!
 host
 (lambda (host context force?) 'application.force-quit))
)");
    EditorApplication application({.path = {},
                                   .initial_text = "source",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});
    application.insert_text(" changed");
    REQUIRE(application.dirty());

    REQUIRE(application.request_close(false));
    CHECK(application.should_quit());
}

TEST_CASE("user initialization owns semantic pointer and scroll behavior") {
    TemporaryFile init(std::format("cind-pointer-policy-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-pointer-policy!
 host
 (lambda (host context event) #f))
(configure-scroll-policy!
 host
 (lambda (host context lines) #f))
)");
    EditorApplication application({.path = {},
                                   .initial_text = "zero\none\n",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});

    CHECK_FALSE(application.handle_pointer({.target = PointerTargetKind::DocumentText,
                                            .window = application.window_id(),
                                            .document_line = 1,
                                            .display_column = 2,
                                            .popup_item = std::nullopt}));
    CHECK(application.session().caret() == TextOffset{});
    CHECK_FALSE(application.handle_scroll({.amount = 1.5, .unit = ScrollUnit::Lines}));
    CHECK(application.session().view().viewport().top_line == 0);
    CHECK(application.session().view().viewport().top_line_offset == doctest::Approx(0.0F));
    CHECK(application.reveal_caret());
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
         .text_command = std::string("edit.self-insert"),
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
         .text_command = std::nullopt,
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
    application.set_pending_command_prefix(
        {.count = 7, .register_name = std::string("a"), .extra = {}});
    CHECK(application.handle_key(KeyStroke::character_key(U'd'), 10));
    CHECK(selected == 3);
    REQUIRE(handled_invocation.arguments.size() == 1);
    CHECK(std::get<std::string>(handled_invocation.arguments.front()) == "captured");
    CHECK(handled_invocation.prefix.count == 7);
    CHECK(handled_invocation.prefix.register_name == std::optional<std::string>{"a"});
    CHECK(application.pending_command_prefix().empty());
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
                                       .text_command = std::nullopt,
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
    CHECK(application.interaction().input_text() == "z");
    CHECK(application.session().snapshot().content().to_string() == "text");
}

TEST_CASE("input states select the command that interprets text") {
    EditorApplication application = make_application("sample.cc", "text");
    EditorRuntime& runtime = application.runtime();
    CommandInvocation received;
    const CommandId command = runtime.commands().define(
        "test.text-policy",
        [&](CommandContext&, const CommandInvocation& invocation) -> CommandResult {
            received = invocation;
            return CommandCompleted{.value = std::nullopt, .selection = CommandSelectionPreserve{}};
        });
    (void)command;
    const InputStateId state =
        runtime.input_states().define({.name = "test-text-policy",
                                       .keymaps = {},
                                       .text_input = TextInputPolicy::Accept,
                                       .text_command = std::string("test.text-policy"),
                                       .cursor = CursorShape::Beam,
                                       .indicator = "T",
                                       .handler = {},
                                       .position_hints = {},
                                       .on_enter = {},
                                       .on_exit = {}});
    runtime.views().set_base_input_state(application.view_id(), state);
    application.set_pending_command_prefix(
        {.count = 3, .register_name = std::string("a"), .extra = {}});

    application.insert_text("é");

    REQUIRE(received.arguments.size() == 1);
    CHECK(std::get<std::string>(received.arguments.front()) == "é");
    CHECK(received.prefix.count == 3);
    CHECK(received.prefix.register_name == std::optional<std::string>{"a"});
    CHECK(application.last_command() == "test.text-policy");
    CHECK(application.pending_command_prefix().empty());
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
    CHECK(application.chrome_content().pending_key == "12");
    CHECK(application.pending_command_prefix().count == 12);
    send_keys(application, "l");
    CHECK(application.session().caret() == TextOffset{12});
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_prefix_text().empty());

    application.session().set_caret(TextOffset{0});
    send_keys(application, "SPC 3");
    send_keys(application, "\"");
    CHECK(application.input_state().name == "input.read-key");
    CHECK(application.chrome_content().pending_key == "3 \"");
    send_keys(application, "a");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.chrome_content().pending_key == "3 \"a");
    CHECK(application.pending_command_prefix().register_name == std::optional<std::string>{"a"});
    send_keys(application, "l");
    CHECK(application.session().caret() == TextOffset{3});
    CHECK(application.chrome_content().pending_key.empty());

    send_keys(application, "- 2 l");
    CHECK(application.session().caret() == TextOffset{1});
    CHECK(application.input_state().name == "meow-normal");

    send_keys(application, "\"");
    CHECK(application.input_state().name == "input.read-key");
    send_keys(application, "C-g");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.chrome_content().pending_key.empty());

    send_keys(application, "\" b");
    CHECK(application.input_state().name == "meow-normal");
    CHECK(application.pending_command_prefix().register_name == std::optional<std::string>{"b"});
    send_keys(application, "l");
    CHECK(application.chrome_content().pending_key.empty());

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
    CHECK(interaction_policy(application).prompt == "Switch buffer: ");
    send_keys(application, "C-g");

    send_keys(application, "m x");
    REQUIRE(application.interaction().active());
    CHECK(interaction_policy(application).prompt == "Command: ");
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
    CHECK(application.chrome_content().pending_key.empty());
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
    const CommandPrefix register_a{.count = std::nullopt, .register_name = "a", .extra = {}};
    CHECK(application.command_loop()
              .execute(require_command(runtime, "edit.copy-region"), context, register_a)
              .status == CommandLoopStatus::Executed);
    CHECK(clipboard == "one\ntwo\n");
    CHECK_FALSE(application.session().active_selection().has_value());

    runtime.views().set_selection(
        application.view_id(),
        ViewSelection{.ranges = {{.anchor = TextOffset{0}, .head = TextOffset{0}},
                                 {.anchor = TextOffset{14}, .head = TextOffset{14}}},
                      .primary = 1,
                      .metadata = "(yank . multi)"});
    CHECK(application.command_loop()
              .execute(require_command(runtime, "edit.yank"), context, register_a)
              .status == CommandLoopStatus::Executed);
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
    CHECK(interaction_policy(application).prompt == "Write file: ");
    CHECK(application.interaction().input_text().empty());
    application.insert_text(path.string());
    send_keys(application, "RET");
    CHECK(application.last_command() == "file.save");

    REQUIRE(wake.wait());
    CHECK(application.poll_background_work());
    CHECK_FALSE(application.has_background_work());
    CHECK(application.buffer_kind(application.session().buffer().id()) == BufferKind::File);
    CHECK(application.buffer_resource(application.session().buffer().id()) == path.string());
    CHECK(application.buffer_name(application.session().buffer().id()) == path.filename().string());
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
        .initial_line = 2,
        .platform_services = {.write_clipboard = {},
                              .read_clipboard = {},
                              .wake_event_loop = [&wake] { wake.notify(); }},
        .init_file = std::nullopt,
    });
    CHECK(application.buffer_kind(application.session().buffer().id()) == BufferKind::Scratch);
    CHECK(application.has_background_work());

    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    CHECK_FALSE(application.has_background_work());
    CHECK(application.buffer_count() == 1);
    CHECK(application.buffer_kind(application.session().buffer().id()) == BufferKind::File);
    CHECK(application.buffer_resource(application.session().buffer().id()) == path.string());
    CHECK(application.session().snapshot().content() == "first\nsecond\nthird\n");
    CHECK(application.session().caret().value == 6);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("scripted open policy round-trips discovered C++ style into the buffer mechanism") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        std::format("cind-application-style-{}", static_cast<long>(::getpid()));
    std::filesystem::create_directories(root);
    {
        std::ofstream style(root / ".clang-format");
        style << "IndentWidth: 7\nTabWidth: 9\n";
    }
    const std::filesystem::path source = root / "sample.cpp";
    {
        std::ofstream output(source);
        output << "int main() {}\n";
    }

    WakeSignal wake;
    EditorApplication application({
        .path = source.string(),
        .initial_text = std::nullopt,
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

    CHECK(application.style_origin() == ".clang-format");
    CHECK(application.session().style().indent_width == 7);
    CHECK(application.session().style().tab_width == 9);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("asynchronous file open snapshots mode policy and skips unrelated style discovery") {
    TemporaryFile source(std::format("cind-application-open-{}.scm", static_cast<long>(::getpid())),
                         "(define value 1)\n");
    WakeSignal wake;
    EditorApplication application({
        .path = source.path().string(),
        .initial_text = std::nullopt,
        .initial_line = 0,
        .platform_services = {.write_clipboard = {},
                              .read_clipboard = {},
                              .wake_event_loop = [&wake] { wake.notify(); }},
        .init_file = std::nullopt,
    });
    const ModeId fundamental =
        application.runtime().modes().find("fundamental-mode").value_or(ModeId{});
    REQUIRE(fundamental);
    application.runtime().resource_policies().define_file_mode("late-test-rule", fundamental,
                                                               {".scm"});

    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    const ModeId major = application.session().buffer().modes().major().value_or(ModeId{});
    CHECK(application.runtime().modes().definition(major).name == "scheme-mode");
    CHECK(application.style_origin() == "plain text");
    CHECK_FALSE(application.syntax_tokens().empty());
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

        const std::optional<ProjectId> project_id =
            application.buffer_project(application.session().buffer().id());
        REQUIRE(project_id.has_value());
        const Project& project = application.runtime().projects().get(*project_id);
        CHECK(project.roots() == std::vector<std::string>{root.string()});
        CHECK(project.files().size() == 2);

        send_keys(application, "C-x p f");
        REQUIRE(application.interaction().state() != nullptr);
        CHECK(interaction_policy(application).provider == "project-files");
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
        CHECK(std::ranges::any_of(application.interaction().state()->candidates,
                                  [](const InteractionCandidate& candidate) {
                                      return candidate.label == "src/watched.cpp";
                                  }));
        application.insert_text("other");
        send_keys(application, "RET");
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.buffer_resource(application.session().buffer().id()) ==
              (root / "src" / "other.cpp").string());

        send_keys(application, "C-x p g");
        REQUIRE(application.interaction().state() != nullptr);
        CHECK(interaction_policy(application).prompt == "Project search: ");
        application.insert_text("int");
        send_keys(application, "RET");
        CHECK(application.project_search_running());
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK_FALSE(application.project_search_running());
        CHECK(application.buffer_kind(application.session().buffer().id()) == BufferKind::Process);
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
        const std::vector<LocationListSnapshot> lists =
            application.location_lists(application.workbench_id());
        REQUIRE(lists.size() == 1);
        CHECK(lists[0].source == "search");
        CHECK(lists[0].materialized_buffer == result_buffer);
        CHECK(lists[0].item_count == 3);
        CHECK(lists[0].current);
        CHECK(application.location_navigation().buffer == result_buffer);
        CHECK_FALSE(application.location_navigation().selected_index.has_value());
        send_keys(application, "M-n");
        CHECK(application.session().caret() == second.source_range.start);
        REQUIRE(application.split_window(WindowSplitAxis::Columns));
        const std::vector<OpenWindowSnapshot> windows = application.open_windows();
        const auto tools_window =
            std::ranges::find_if(windows, [](const OpenWindowSnapshot& window) {
                return window.role == std::optional<std::string>{"tools"};
            });
        REQUIRE(tools_window != windows.end());
        const std::string result_text = application.session().snapshot().content().to_string();
        application.insert_text("ignored");
        CHECK(application.session().snapshot().content().to_string() == result_text);
        CHECK(application.message() == "buffer is read-only");
        send_keys(application, "RET");
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.buffer_resource(application.session().buffer().id()) == second.resource);
        CHECK(application.session().snapshot().content().position(application.session().caret()) ==
              resolve_line_position(application.session().snapshot().content(), second.target)
                  .value());
        CHECK(application.location_navigation().selected_index == 1);
        CHECK(application.buffer_id(tools_window->window) == result_buffer);
        CHECK(application.session(tools_window->window).caret() == second.source_range.start);

        application.session().set_caret(TextOffset{});
        application.session().insert_text("prefix\n");

        send_keys(application, "M-g n");
        while (application.has_background_work()) {
            REQUIRE(wake.wait());
            (void)application.poll_background_work();
        }
        CHECK(application.buffer_resource(application.session().buffer().id()) == third.resource);
        CHECK(application.session().snapshot().content().position(application.session().caret()) ==
              resolve_line_position(application.session().snapshot().content(), third.target)
                  .value());
        CHECK(application.location_navigation().selected_index == 2);

        send_keys(application, "M-g p");
        CHECK(application.buffer_resource(application.session().buffer().id()) == second.resource);
        CHECK(application.session().snapshot().content().position(application.session().caret()) ==
              LinePosition{.line = second.target.line + 1, .byte_column = second.target.column});
        CHECK(application.location_navigation().selected_index == 1);
        REQUIRE(application.switch_buffer(result_buffer));
        CHECK(application.session().caret() == second.source_range.start);
        send_keys(application, "C-x `");
        CHECK(application.location_navigation().selected_index == 2);

        CommandContext kill_results(application.runtime(), tools_window->window, result_buffer,
                                    application.view_id(tools_window->window));
        const CommandId force_kill_results =
            application.runtime().commands().find("buffer.force-kill").value_or(CommandId{});
        REQUIRE(force_kill_results);
        REQUIRE(
            application.runtime().commands().invoke(force_kill_results, kill_results).has_value());
        CHECK_FALSE(application.location_navigation().buffer.has_value());
        CHECK(application.location_navigation().location_count == 3);
        send_keys(application, "M-g p");
        CHECK(application.buffer_resource(application.session().buffer().id()) == second.resource);
        CHECK(application.location_navigation().selected_index == 1);
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("default keymap follows Emacs movement search undo and prefix conventions") {
    EditorApplication application = make_application("sample.cc", "one two one");

    send_keys(application, "C-f");
    CHECK(application.session().caret().value == 1);
    send_keys(application, "C-s");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "search: ");
    send_keys(application, "C-g");
    CHECK_FALSE(application.interaction().active());

    send_keys(application, "C-r");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "search backward: ");
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

TEST_CASE("workbench switching preserves inactive window and view state") {
    EditorApplication application = make_application("sample.cc", "one\ntwo\nthree\n");
    const WorkbenchId first_workbench = application.workbench_id();
    const WindowId first_window = application.window_id();
    application.session().set_caret(TextOffset{4});
    application.session().view().viewport().top_line = 1;
    REQUIRE(application.split_window(WindowSplitAxis::Columns));
    const WindowId first_other_window = application.window_layout().leaves().back();
    REQUIRE(application.focus_window(first_other_window));
    application.session().set_caret(TextOffset{8});

    const WorkbenchId second_workbench = application.create_workbench("notes");
    CHECK(application.workbench_id() == second_workbench);
    CHECK(application.window_layout().leaves().size() == 1);
    const WindowId second_window = application.window_id();
    CHECK(second_window != first_window);
    CHECK(second_window != first_other_window);
    CHECK(application.session().caret() == TextOffset{8});
    application.session().set_caret(TextOffset{0});
    application.session().view().viewport().top_line = 0;

    send_keys(application, "C-h b");
    CHECK(application.buffer_name(application.session().buffer().id()) == "*Help*");
    const std::vector<BufferId> second_buffers = application.workbench_buffers(second_workbench);
    REQUIRE(second_buffers.size() == 2);
    CHECK(second_buffers.front() == application.buffer_id());

    REQUIRE(application.switch_workbench(first_workbench));
    CHECK(application.window_id() == first_other_window);
    CHECK(application.window_layout().leaves().size() == 2);
    CHECK(application.session().caret() == TextOffset{8});
    CHECK(application.runtime().windows().try_get(second_window) != nullptr);
    CHECK(application.workbench_buffers(first_workbench).size() == 1);

    REQUIRE(application.switch_workbench(second_workbench));
    CHECK(application.buffer_name(application.session().buffer().id()) == "*Help*");
    CHECK(application.session().caret() == TextOffset{0});
    REQUIRE(application.close_workbench(second_workbench));
    CHECK(application.workbench_id() == first_workbench);
    CHECK(application.runtime().windows().try_get(second_window) == nullptr);
    CHECK(application.runtime().windows().try_get(first_window) != nullptr);
    CHECK(application.runtime().windows().try_get(first_other_window) != nullptr);
    CHECK(application.workbench_snapshots().size() == 1);
    CHECK_FALSE(application.close_workbench(first_workbench));
}

TEST_CASE("three workbenches retain independent layouts and recency") {
    EditorApplication application = make_application("sample.cc", "one\ntwo\n");
    const BufferId source = application.buffer_id();
    const WorkbenchId code = application.workbench_id();
    REQUIRE(application.split_window(WindowSplitAxis::Columns));

    const WorkbenchId notes = application.create_workbench("notes");
    send_keys(application, "C-h b");
    const BufferId help = application.buffer_id();
    CHECK(help != source);
    REQUIRE(application.window_layout().leaves().size() == 2);

    const WorkbenchId scratch = application.create_workbench("scratch");
    CHECK(application.window_layout().leaves().size() == 1);
    CHECK(application.workbench_buffers(scratch) == std::vector<BufferId>{help});

    REQUIRE(application.switch_workbench(code));
    CHECK(application.window_layout().leaves().size() == 2);
    CHECK(application.workbench_buffers(code) == std::vector<BufferId>{source});

    REQUIRE(application.switch_workbench(notes));
    CHECK(application.window_layout().leaves().size() == 2);
    CHECK(application.buffer_id() == help);
    CHECK(application.workbench_buffers(notes) == std::vector<BufferId>{help, source});

    REQUIRE(application.switch_workbench(scratch));
    CHECK(application.window_layout().leaves().size() == 1);
    CHECK(application.buffer_id() == help);
    const std::vector<WorkbenchSnapshot> snapshots = application.workbench_snapshots();
    REQUIRE(snapshots.size() == 3);
    CHECK(std::ranges::any_of(snapshots, [](const WorkbenchSnapshot& workbench) {
        return workbench.name.empty() && workbench.windows.size() == 2;
    }));
    CHECK(std::ranges::any_of(snapshots, [](const WorkbenchSnapshot& workbench) {
        return workbench.name == "notes" && workbench.windows.size() == 2;
    }));
    CHECK(std::ranges::any_of(snapshots, [](const WorkbenchSnapshot& workbench) {
        return workbench.name == "scratch" && workbench.active && workbench.windows.size() == 1;
    }));
}

TEST_CASE("workbench project scope unifies repositories without leaking into other workbenches") {
    TemporaryDirectory directory(
        std::format("cind-workbench-scope-{}", static_cast<long>(::getpid())));
    const std::filesystem::path backend = directory.path() / "backend";
    const std::filesystem::path frontend = directory.path() / "frontend";
    std::filesystem::create_directories(backend / ".git");
    std::filesystem::create_directories(backend / "src");
    std::filesystem::create_directories(frontend / ".git");
    std::filesystem::create_directories(frontend / "src");
    const std::filesystem::path backend_source =
        directory.write("backend/src/server.cc", "int server() { return 1; }\n");
    const std::filesystem::path frontend_source =
        directory.write("frontend/src/client.cc", "int client() { return 2; }\n");

    WakeSignal wake;
    EditorApplication application({
        .path = backend_source.string(),
        .initial_text = std::nullopt,
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
    const BufferId backend_buffer = application.buffer_id();
    const ProjectId backend_project =
        application.buffer_project(backend_buffer).value_or(ProjectId{});
    REQUIRE(backend_project);

    REQUIRE(application.open_file(frontend_source.string()).has_value());
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    const BufferId frontend_buffer = application.buffer_id();
    const ProjectId frontend_project =
        application.buffer_project(frontend_buffer).value_or(ProjectId{});
    REQUIRE(frontend_project);
    CHECK(frontend_project != backend_project);

    const WorkbenchId shop = application.create_workbench("shop", frontend_project);
    REQUIRE(application.adopt_project(shop, backend_project));
    const std::vector<BufferId> shop_buffers = application.workbench_buffers(shop);
    REQUIRE(shop_buffers.size() == 2);
    CHECK(shop_buffers.front() == frontend_buffer);
    CHECK(std::ranges::find(shop_buffers, backend_buffer) != shop_buffers.end());

    send_keys(application, "C-x p f");
    REQUIRE(application.interaction().state() != nullptr);
    const std::vector<InteractionCandidate>& candidates =
        application.interaction().state()->candidates;
    CHECK(std::ranges::any_of(candidates, [&](const InteractionCandidate& candidate) {
        return candidate.value == backend_source.string() && candidate.detail == backend.string();
    }));
    CHECK(std::ranges::any_of(candidates, [&](const InteractionCandidate& candidate) {
        return candidate.value == frontend_source.string() && candidate.detail == frontend.string();
    }));
    send_keys(application, "C-g");

    const WorkbenchId isolated = application.create_workbench("cind");
    const std::vector<BufferId> isolated_buffers = application.workbench_buffers(isolated);
    CHECK(isolated_buffers == std::vector<BufferId>{frontend_buffer});
    CHECK(std::ranges::find(isolated_buffers, backend_buffer) == isolated_buffers.end());
    REQUIRE(application.switch_workbench(shop));
    CHECK(application.workbench_buffers(shop).size() == 2);

    const std::vector<WorkbenchSnapshot> snapshots = application.workbench_snapshots();
    const auto shop_snapshot =
        std::ranges::find(snapshots, std::string{"shop"}, &WorkbenchSnapshot::name);
    const auto isolated_snapshot =
        std::ranges::find(snapshots, std::string{"cind"}, &WorkbenchSnapshot::name);
    REQUIRE(shop_snapshot != snapshots.end());
    REQUIRE(isolated_snapshot != snapshots.end());
    CHECK(shop_snapshot->scope == std::vector<ProjectId>{frontend_project, backend_project});
    CHECK(isolated_snapshot->scope.empty());
}

TEST_CASE("workbench session capture uses stable resources and layout topology") {
    EditorApplication application = make_application("/tmp/source.cc", "one\ntwo\n");
    const WorkbenchId first = application.workbench_id();
    const WindowId first_window = application.window_id();
    application.session().set_caret(TextOffset{4});
    const std::optional<JumpNodeId> first_jump = application.mark_jump(first_window);
    REQUIRE(first_jump.has_value());
    REQUIRE(application.split_window(WindowSplitAxis::Rows));
    const WindowId tool_window = application.window_layout().leaves().back();
    application.session(tool_window).set_caret(TextOffset{0});
    const std::optional<JumpNodeId> tool_jump = application.mark_jump(tool_window);
    REQUIRE(tool_jump.has_value());
    REQUIRE(application.link_jump(tool_window, *first_jump, *tool_jump, "manual", true));
    REQUIRE(application.set_window_role(tool_window, "tools").has_value());
    REQUIRE(application.set_window_pinned(first_window, true).has_value());
    const ProjectId project =
        application.runtime().projects().create({.name = "source",
                                                 .roots = {"/tmp/source"},
                                                 .discovery_provider = "session",
                                                 .discovery_marker = {}});
    REQUIRE(application.adopt_project(first, project));
    const WorkbenchId second = application.create_workbench("notes");

    const std::expected<WorkbenchSessionState, std::string> captured =
        parse_workbench_session(application.serialize_workbench_session());
    REQUIRE(captured.has_value());
    REQUIRE(captured->workbenches.size() == 2);
    CHECK(captured->active_workbench == 1);
    CHECK(captured->workbenches[0].scope_roots == std::vector<std::string>{"/tmp/source"});
    CHECK(captured->workbenches[0].mru_resources == std::vector<std::string>{"/tmp/source.cc"});
    CHECK_FALSE(captured->workbenches[0].layout.leaf());
    CHECK(captured->workbenches[0].layout.axis == WindowSplitAxis::Rows);
    REQUIRE(captured->workbenches[0].layout.first->window.has_value());
    CHECK(captured->workbenches[0].layout.first->window->caret == 4);
    CHECK(captured->workbenches[0].layout.first->window->pinned);
    REQUIRE(captured->workbenches[0].layout.second->window.has_value());
    CHECK(captured->workbenches[0].layout.second->window->role ==
          std::optional<std::string>{"tools"});
    REQUIRE(captured->workbenches[0].jump_nodes.size() == 2);
    REQUIRE(captured->workbenches[0].jump_edges.size() == 1);
    CHECK(captured->workbenches[0].jump_edges[0].persistent);
    CHECK(captured->workbenches[0].layout.first->window->jump_walk ==
          std::vector<JumpNodeId>{*first_jump});
    CHECK(captured->workbenches[0].layout.second->window->jump_walk ==
          std::vector<JumpNodeId>{*first_jump, *tool_jump});
    CHECK(captured->workbenches[1].name == "notes");
    CHECK(application.workbench_id() == second);
}

TEST_CASE("workbench session restore rebuilds durable state and tolerates missing resources") {
    TemporaryDirectory directory(
        std::format("cind-workbench-restore-{}", static_cast<long>(::getpid())));
    const std::filesystem::path main = directory.write("main.cc", "zero\none\ntwo\n");
    const std::filesystem::path visitor = directory.write("notes.txt", "visitor\n");
    const std::filesystem::path missing = directory.path() / "missing.cc";

    const auto leaf = [](std::optional<std::string> resource, std::uint32_t caret,
                         std::optional<std::string> role = std::nullopt, bool pinned = false,
                         bool created = false) {
        return WorkbenchLayoutSessionState{
            .window = WorkbenchWindowSessionState{.resource = std::move(resource),
                                                  .caret = caret,
                                                  .role = std::move(role),
                                                  .pinned = pinned,
                                                  .created_by_policy = created},
            .axis = WindowSplitAxis::Rows,
            .ratio = 0.5F,
            .first = nullptr,
            .second = nullptr};
    };
    WorkbenchSessionState state;
    state.active_workbench = 1;
    WorkbenchSessionEntry code;
    code.name = "code";
    code.scope_roots = {directory.path().string()};
    code.mru_resources = {visitor.string(), main.string(), missing.string()};
    code.active_leaf = 1;
    code.layout = {.window = std::nullopt,
                   .axis = WindowSplitAxis::Rows,
                   .ratio = 0.35F,
                   .first = std::make_unique<WorkbenchLayoutSessionState>(
                       leaf(main.string(), 5, std::nullopt, true)),
                   .second = std::make_unique<WorkbenchLayoutSessionState>(
                       leaf(missing.string(), 80, "tools", false, true))};
    code.jump_nodes = {{.id = 10,
                        .resource = main.string(),
                        .fallback = {.line = 1, .byte_column = 0},
                        .excerpt = "one",
                        .created_at = 1,
                        .last_visit = 3},
                       {.id = 11,
                        .resource = visitor.string(),
                        .fallback = {.line = 0, .byte_column = 2},
                        .excerpt = "visitor",
                        .created_at = 2,
                        .last_visit = 4}};
    code.jump_edges = {{.from = 10, .to = 11, .kind = "manual", .at = 5, .persistent = true}};
    code.layout.first->window->jump_walk = {10, 11};
    code.layout.first->window->jump_cursor = 0;
    WorkbenchSessionEntry notes;
    notes.name = "notes";
    notes.mru_resources = {visitor.string()};
    notes.layout = leaf(visitor.string(), 3);
    state.workbenches.push_back(std::move(code));
    state.workbenches.push_back(std::move(notes));

    WakeSignal wake;
    EditorApplication application = make_application(
        {}, "fallback\n", {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                               wake.notify();
                           }});
    const WorkbenchId original_workbench = application.workbench_id();
    const WindowId original_window = application.window_id();
    CHECK_FALSE(application.restore_workbench_session("not a session").has_value());
    CHECK(application.workbench_id() == original_workbench);

    REQUIRE(application.restore_workbench_session(serialize_workbench_session(state)).has_value());
    CHECK(application.runtime().windows().try_get(original_window) == nullptr);
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }

    const std::vector<WorkbenchSnapshot> snapshots = application.workbench_snapshots();
    REQUIRE(snapshots.size() == 2);
    const auto code_snapshot =
        std::ranges::find(snapshots, std::string{"code"}, &WorkbenchSnapshot::name);
    const auto notes_snapshot =
        std::ranges::find(snapshots, std::string{"notes"}, &WorkbenchSnapshot::name);
    REQUIRE(code_snapshot != snapshots.end());
    REQUIRE(notes_snapshot != snapshots.end());
    CHECK_FALSE(code_snapshot->active);
    CHECK(notes_snapshot->active);
    REQUIRE(notes_snapshot->mru.size() == 1);
    CHECK(application.buffer_resource(notes_snapshot->mru.front()) ==
          std::optional{visitor.string()});
    CHECK(application.session().caret() == TextOffset{3});

    REQUIRE(code_snapshot->scope.size() == 1);
    CHECK(application.runtime().projects().get(code_snapshot->scope.front()).roots() ==
          std::vector<std::string>{directory.path().string()});
    REQUIRE(code_snapshot->mru.size() == 3);
    CHECK(application.buffer_resource(code_snapshot->mru[0]) ==
          std::optional{visitor.string()});
    CHECK(application.buffer_resource(code_snapshot->mru[1]) ==
          std::optional{main.string()});
    CHECK_FALSE(application.buffer_resource(code_snapshot->mru[2]));

    REQUIRE(application.switch_workbench(code_snapshot->workbench));
    REQUIRE(application.window_layout().root() != nullptr);
    CHECK_FALSE(application.window_layout().root()->leaf());
    CHECK(application.window_layout().root()->axis == WindowSplitAxis::Rows);
    CHECK(application.window_layout().root()->ratio == doctest::Approx(0.35F));
    REQUIRE(application.open_windows().size() == 2);
    const WindowId source_window = application.window_layout().leaves().front();
    const WindowId tool_window = application.window_layout().leaves().back();
    CHECK(
        application.buffer_resource(application.buffer_id(source_window)) ==
        std::optional{main.string()});
    CHECK(application.session(source_window).caret() == TextOffset{5});
    CHECK(application.window_snapshot(source_window).pinned);
    const std::optional<JumpNode> restored_jump = application.jump_node(source_window, 10);
    REQUIRE(restored_jump.has_value());
    CHECK(restored_jump->position.buffer == application.buffer_id(source_window));
    CHECK(restored_jump->position.anchor != 0);
    REQUIRE(application.jump_branches(source_window).size() == 1);
    CHECK(application.jump_branches(source_window)[0].to == 11);
    CHECK(application.window_id() == tool_window);
    CHECK(application.window_snapshot(tool_window).role == std::optional<std::string>{"tools"});
    CHECK(application.window_snapshot(tool_window).created_by_policy);
    CHECK_FALSE(application.buffer_id_by_resource(missing.string()));
    CHECK(application.message() == "workbench session restored · 1 resources unavailable");
}

TEST_CASE("workbench session validation preserves current state on invalid durable identity") {
    EditorApplication application = make_application("sample.cc", "one\n");
    const WorkbenchId original_workbench = application.workbench_id();
    const WindowId original_window = application.window_id();
    const std::size_t original_projects = application.runtime().projects().all().size();

    const auto leaf = [](std::optional<std::string> resource,
                         std::optional<std::string> role = std::nullopt) {
        return WorkbenchLayoutSessionState{
            .window = WorkbenchWindowSessionState{.resource = std::move(resource),
                                                  .caret = 0,
                                                  .role = std::move(role),
                                                  .pinned = false,
                                                  .created_by_policy = false},
            .axis = WindowSplitAxis::Rows,
            .ratio = 0.5F,
            .first = nullptr,
            .second = nullptr};
    };
    WorkbenchSessionState state;
    WorkbenchSessionEntry invalid;
    invalid.name = "invalid";
    invalid.scope_roots = {"/tmp/project"};
    invalid.mru_resources = {"/tmp/source.cc"};
    invalid.layout = {
        .window = std::nullopt,
        .axis = WindowSplitAxis::Rows,
        .ratio = 0.5F,
        .first = std::make_unique<WorkbenchLayoutSessionState>(leaf("/tmp/source.cc", "tools")),
        .second = std::make_unique<WorkbenchLayoutSessionState>(leaf("/tmp/results", "tools"))};
    state.workbenches.push_back(std::move(invalid));

    const std::expected<void, std::string> restored =
        application.restore_workbench_session(serialize_workbench_session(state));
    REQUIRE_FALSE(restored.has_value());
    CHECK(restored.error().find("workbench session contains an invalid or duplicate window role") !=
          std::string::npos);
    CHECK(application.workbench_id() == original_workbench);
    CHECK(application.window_id() == original_window);
    CHECK(application.runtime().windows().try_get(original_window) != nullptr);
    CHECK(application.runtime().projects().all().size() == original_projects);
}

TEST_CASE("workbench session restoration isolates staging names from durable names") {
    EditorApplication application = make_application({}, "fallback\n");
    WorkbenchSessionState state;
    state.active_workbench = 1;
    const auto leaf = [] {
        return WorkbenchLayoutSessionState{
            .window = WorkbenchWindowSessionState{.resource = std::nullopt,
                                                  .caret = 0,
                                                  .role = std::nullopt,
                                                  .pinned = false,
                                                  .created_by_policy = false},
            .axis = WindowSplitAxis::Rows,
            .ratio = 0.5F,
            .first = nullptr,
            .second = nullptr};
    };
    state.workbenches.push_back({.name = " *restore-1-0*",
                                 .scope_roots = {},
                                 .mru_resources = {},
                                 .layout = leaf(),
                                 .active_leaf = 0});
    state.workbenches.push_back({.name = "notes",
                                 .scope_roots = {},
                                 .mru_resources = {},
                                 .layout = leaf(),
                                 .active_leaf = 0});

    REQUIRE(application.restore_workbench_session(serialize_workbench_session(state)).has_value());
    const std::vector<WorkbenchSnapshot> restored = application.workbench_snapshots();
    REQUIRE(restored.size() == 2);
    CHECK(std::ranges::any_of(restored, [](const WorkbenchSnapshot& workbench) {
        return workbench.name == " *restore-1-0*" && !workbench.active;
    }));
    CHECK(std::ranges::any_of(restored, [](const WorkbenchSnapshot& workbench) {
        return workbench.name == "notes" && workbench.active;
    }));
}

TEST_CASE("workbench session commands persist and restore through the async runtime") {
    TemporaryDirectory directory(
        std::format("cind-workbench-command-{}", static_cast<long>(::getpid())));
    const std::filesystem::path source = directory.write("main.cc", "int main() {}\n");
    const std::filesystem::path session_path = directory.path() / "workbench.session";
    WakeSignal wake;
    EditorApplication application =
        make_application(source.string(), "int main() {}\n",
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    const WorkbenchId first = application.workbench_id();
    REQUIRE(application.split_window(WindowSplitAxis::Columns));
    const WorkbenchId notes = application.create_workbench("notes");

    send_keys(application, "C-x w S");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "Save workbench session: ");
    application.insert_text(session_path.string());
    send_keys(application, "RET");
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    CHECK(std::filesystem::is_regular_file(session_path));
    CHECK(application.message() ==
          std::format("saved workbench session {}", session_path.string()));

    REQUIRE(application.close_workbench(notes));
    CHECK(application.workbench_id() == first);
    CHECK(application.workbench_snapshots().size() == 1);

    send_keys(application, "C-x w R");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "Restore workbench session: ");
    application.insert_text(session_path.string());
    send_keys(application, "RET");
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }

    const std::vector<WorkbenchSnapshot> restored = application.workbench_snapshots();
    REQUIRE(restored.size() == 2);
    CHECK(std::ranges::any_of(restored, [](const WorkbenchSnapshot& workbench) {
        return workbench.name == "notes" && workbench.active;
    }));
    CHECK(std::ranges::any_of(restored, [](const WorkbenchSnapshot& workbench) {
        return workbench.name.empty() && workbench.windows.size() == 2;
    }));
    CHECK(application.message() == "workbench session restored");
}

TEST_CASE("window roles pinning and policy provenance stay frontend independent") {
    EditorApplication application = make_application("sample.cc", "one\n");
    const WindowId first = application.window_id();
    REQUIRE(application.split_window(WindowSplitAxis::Columns));
    const WindowId second = application.window_layout().leaves().back();

    REQUIRE(application.set_window_role(second, "tools").has_value());
    CHECK(application.workbench_slot(application.workbench_id(), "tools") == std::optional{second});
    REQUIRE(application.set_window_pinned(second, true).has_value());
    REQUIRE(application.set_window_created_by_policy(second, true).has_value());
    const std::vector<OpenWindowSnapshot> windows = application.open_windows();
    const auto snapshot = std::ranges::find(windows, second, &OpenWindowSnapshot::window);
    REQUIRE(snapshot != windows.end());
    CHECK(snapshot->role == std::optional<std::string>{"tools"});
    CHECK(snapshot->pinned);
    CHECK(snapshot->created_by_policy);

    REQUIRE(application.set_window_role(first, "tools").has_value());
    CHECK(application.workbench_slot(application.workbench_id(), "tools") == std::optional{first});
    CHECK_FALSE(application.window_snapshot(second).role.has_value());
    REQUIRE(application.set_window_role(first, std::nullopt).has_value());
    CHECK_FALSE(application.workbench_slot(application.workbench_id(), "tools").has_value());

    REQUIRE(application.focus_window(second));
    send_keys(application, "C-x w r");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).provider == "window-roles");
    application.insert_text("doc");
    send_keys(application, "RET");
    CHECK(application.workbench_slot(application.workbench_id(), "doc") == std::optional{second});
    send_keys(application, "C-x w p");
    CHECK_FALSE(application.window_snapshot(second).pinned);
    send_keys(application, "C-x w d");
    CHECK(application.runtime().windows().try_get(second) == nullptr);
    CHECK_FALSE(application.workbench_slot(application.workbench_id(), "doc").has_value());
}

TEST_CASE("display intents reuse named slots and route jumps around pinned windows") {
    EditorApplication application = make_application("sample.cc", "one\n");
    const WindowId edit_window = application.window_id();
    const BufferId edit_buffer = application.buffer_id();

    send_keys(application, "C-h b");
    REQUIRE(application.open_windows().size() == 2);
    const WindowId doc_window = application.window_id();
    const BufferId help_buffer = application.buffer_id();
    CHECK(doc_window != edit_window);
    CHECK(application.window_snapshot(doc_window).role == std::optional<std::string>{"doc"});
    CHECK(application.window_snapshot(doc_window).created_by_policy);
    CHECK(application.buffer_id(edit_window) == edit_buffer);

    REQUIRE(application.set_window_role(doc_window, "explicit").has_value());
    const std::expected<WindowId, std::string> explicit_target =
        application.display_buffer(edit_buffer, "explicit", edit_window);
    REQUIRE(explicit_target.has_value());
    CHECK(*explicit_target == edit_window);
    REQUIRE(application.set_window_role(doc_window, "doc").has_value());

    send_keys(application, "C-h b");
    CHECK(application.open_windows().size() == 2);
    CHECK(application.window_id() == doc_window);
    CHECK(std::ranges::any_of(application.active_keymap_layers(), [&](const KeymapLayer& layer) {
        return application.runtime().keymaps().definition(layer.keymap).name ==
                   "window.policy-created" &&
               layer.scope == "window:policy-created";
    }));
    send_keys(application, "M-x");
    REQUIRE(application.interaction().active());
    CHECK(std::ranges::none_of(application.active_keymap_layers(), [&](const KeymapLayer& layer) {
        return application.runtime().keymaps().definition(layer.keymap).name ==
               "window.policy-created";
    }));
    send_keys(application, "C-g");
    send_keys(application, "q");
    CHECK(application.open_windows().size() == 1);
    CHECK(application.window_id() == edit_window);

    REQUIRE(application.split_window(WindowSplitAxis::Columns));
    const WindowId jump_window = application.window_layout().leaves().back();
    REQUIRE(application.set_window_role(jump_window, "jump").has_value());
    REQUIRE(application.set_window_pinned(edit_window, true).has_value());
    const std::expected<WindowId, std::string> target =
        application.display_buffer(help_buffer, "jump", edit_window);
    REQUIRE(target.has_value());
    CHECK(*target == jump_window);
    CHECK(application.window_id() == jump_window);
    CHECK(application.buffer_id(edit_window) == edit_buffer);
    CHECK(application.buffer_id(jump_window) == help_buffer);
}

TEST_CASE("display navigation records a window walk with exact landing positions") {
    EditorApplication application = make_application("sample.cc", "zero\none\ntwo\n");
    const WindowId origin = application.window_id();
    const BufferId source = application.buffer_id();

    send_keys(application, "C-h b");
    const WindowId navigation_window = application.window_id();
    const BufferId help = application.buffer_id();
    REQUIRE(navigation_window != origin);

    REQUIRE(application.navigate_jump(navigation_window, -1));
    CHECK(application.buffer_id(navigation_window) == source);
    CHECK(application.session(navigation_window)
              .snapshot()
              .content()
              .position(application.session(navigation_window).caret())
              .line == 0);
    REQUIRE(application.navigate_jump(navigation_window, 1));
    CHECK(application.buffer_id(navigation_window) == help);

    const std::expected<WindowId, std::string> displayed = application.display_buffer(
        source, "definition", navigation_window, LinePosition{.line = 2, .byte_column = 1});
    REQUIRE(displayed == std::expected<WindowId, std::string>{navigation_window});
    CHECK(application.session(navigation_window)
              .snapshot()
              .content()
              .position(application.session(navigation_window).caret()) ==
          LinePosition{.line = 2, .byte_column = 1});
    REQUIRE(application.navigate_jump(navigation_window, -1));
    CHECK(application.buffer_id(navigation_window) == help);
    REQUIRE(application.navigate_jump(navigation_window, 1));
    CHECK(application.session(navigation_window)
              .snapshot()
              .content()
              .position(application.session(navigation_window).caret()) ==
          LinePosition{.line = 2, .byte_column = 1});
    send_keys(application, "M-,");
    CHECK(application.buffer_id(navigation_window) == help);
    send_keys(application, "C-M-,");
    CHECK(application.buffer_id(navigation_window) == source);
}

TEST_CASE("jump replay asynchronously reopens a released buffer without recording itself") {
    TemporaryDirectory directory(std::format("cind-jump-reopen-{}", static_cast<long>(::getpid())));
    const std::filesystem::path source_path = directory.write("source.cc", "source\n");
    const std::filesystem::path target_path = directory.write("target.cc", "target\n");
    WakeSignal wake;
    EditorApplication application =
        make_application(source_path.string(), "source\n",
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    const WindowId window = application.window_id();
    const BufferId source = application.buffer_id();

    REQUIRE(application.open_file(target_path.string()).has_value());
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    const BufferId target = application.buffer_id();
    REQUIRE(target != source);
    REQUIRE(application.navigate_jump(window, -1));
    CHECK(application.buffer_id() == source);
    REQUIRE(application.release_buffer(target, source).has_value());
    CHECK_FALSE(application.buffer_id_by_resource(target_path.string()));

    REQUIRE(application.navigate_jump(window, 1));
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    CHECK(application.buffer_resource(application.buffer_id()) ==
          std::optional{target_path.string()});
    REQUIRE(application.navigate_jump(window, -1));
    CHECK(application.buffer_id() == source);
    REQUIRE(application.navigate_jump(window, 1));
    CHECK(application.buffer_resource(application.buffer_id()) ==
          std::optional{target_path.string()});
}

TEST_CASE("invalid display plans fall back to the default Scheme policy") {
    TemporaryFile init(std::format("cind-display-fallback-{}.scm", static_cast<long>(::getpid())),
                       R"((configure-display-policy!
 host
 (lambda (host facts)
   (vector 'display-reuse (vector-ref facts 2))))
)");
    EditorApplication application({.path = "sample.cc",
                                   .initial_text = "one\n",
                                   .initial_line = 0,
                                   .platform_services = {},
                                   .init_file = init.path().string()});
    const WindowId edit_window = application.window_id();
    REQUIRE(application.set_window_pinned(edit_window, true).has_value());

    send_keys(application, "C-h b");

    REQUIRE(application.open_windows().size() == 2);
    CHECK(application.window_id() != edit_window);
    CHECK(application.buffer_name(application.session().buffer().id()) == "*Help*");
    CHECK(application.window_snapshot(application.window_id()).role ==
          std::optional<std::string>{"doc"});
}

TEST_CASE("workbench commands manage named editing surfaces") {
    EditorApplication application = make_application("sample.cc", "one\n");
    const WorkbenchId initial = application.workbench_id();
    CHECK(application.workbench_snapshots().size() == 1);
    CHECK(std::ranges::none_of(
        application.modeline(application.window_id()).segments,
        [](const ModelineSegment& segment) { return segment.text == "default"; }));

    send_keys(application, "C-x w n");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "New workbench: ");
    application.insert_text("notes");
    send_keys(application, "RET");

    REQUIRE(application.workbench_snapshots().size() == 2);
    const WorkbenchId notes = application.workbench_id();
    CHECK(notes != initial);
    CHECK(std::ranges::any_of(
        application.modeline(application.window_id()).segments, [](const ModelineSegment& segment) {
            return segment.text == "notes" && segment.tone == ModelineTone::Salient;
        }));

    send_keys(application, "C-x w s");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).provider == "workbenches");
    CHECK(application.interaction().state()->candidates.size() == 2);
    application.insert_text("default");
    send_keys(application, "RET");
    CHECK(application.workbench_id() == initial);

    send_keys(application, "C-x w k");
    CHECK(application.workbench_id() == notes);
    CHECK(application.workbench_snapshots().size() == 1);
    CHECK(std::ranges::none_of(
        application.modeline(application.window_id()).segments,
        [](const ModelineSegment& segment) { return segment.text == "notes"; }));
}

TEST_CASE("buffer picker defaults to the active workbench and can widen globally") {
    WakeSignal wake;
    EditorApplication application =
        make_application("sample.cc", "one\n",
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    const BufferId initial = application.buffer_id();
    TemporaryFile first_only_file(
        std::format("cind-workbench-first-only-{}.txt", static_cast<long>(::getpid())), "one\n");
    REQUIRE(application.open_file(first_only_file.path().string()).has_value());
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    const BufferId first_only = application.buffer_id();
    const std::string first_only_name =
        application.buffer_name(application.session().buffer().id());
    REQUIRE(application.switch_buffer(initial));

    const WorkbenchId second = application.create_workbench("second");
    send_keys(application, "C-h b");
    const BufferId help = application.buffer_id();
    CHECK(help != initial);
    const std::vector<BufferId> second_buffers = application.workbench_buffers(second);
    CHECK(std::ranges::find(second_buffers, first_only) == second_buffers.end());

    send_keys(application, "C-x b");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).provider == "buffers");
    CHECK(std::ranges::none_of(
        application.interaction().state()->candidates,
        [&](const InteractionCandidate& candidate) { return candidate.value == first_only_name; }));

    send_keys(application, "C-x b");
    CHECK(interaction_policy(application).provider == "buffers-global");
    CHECK(std::ranges::any_of(
        application.interaction().state()->candidates,
        [&](const InteractionCandidate& candidate) { return candidate.value == first_only_name; }));
    send_keys(application, "C-x b");
    CHECK(interaction_policy(application).provider == "buffers");
    send_keys(application, "C-g");

    CHECK(application.buffer_id() == help);
    send_keys(application, "C-x b C-x b");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).provider == "buffers-global");
    application.insert_text(first_only_name);
    send_keys(application, "RET");
    CHECK(application.buffer_id() == first_only);
    CHECK(application.workbench_snapshots().back().scope.empty());

    send_keys(application, "C-x b");
    REQUIRE(application.interaction().state() != nullptr);
    const auto visitor = std::ranges::find(application.interaction().state()->candidates,
                                           first_only_name, &InteractionCandidate::value);
    REQUIRE(visitor != application.interaction().state()->candidates.end());
    CHECK(visitor->detail.ends_with("visitor"));
    send_keys(application, "C-g");

    send_keys(application, "C-x Right");
    CHECK(application.buffer_id() == help);
    send_keys(application, "C-x Right");
    CHECK(application.buffer_id() == initial);
    send_keys(application, "C-x Right");
    CHECK(application.buffer_id() == first_only);
    send_keys(application, "C-x Left");
    CHECK(application.buffer_id() == initial);

    REQUIRE(application.switch_buffer(first_only));
    send_keys(application, "C-x w e");
    send_keys(application, "C-x b");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(std::ranges::none_of(
        application.interaction().state()->candidates,
        [&](const InteractionCandidate& candidate) { return candidate.value == first_only_name; }));
    send_keys(application, "C-x b");
    CHECK(std::ranges::any_of(
        application.interaction().state()->candidates,
        [&](const InteractionCandidate& candidate) { return candidate.value == first_only_name; }));
    send_keys(application, "C-g");
}

TEST_CASE("asynchronous display completion stays with its origin workbench") {
    TemporaryFile source(
        std::format("cind-workbench-async-origin-{}.txt", static_cast<long>(::getpid())),
        "visitor\n");
    WakeSignal wake;
    EditorApplication application =
        make_application("sample.cc", "one\n",
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    const WorkbenchId origin_workbench = application.workbench_id();
    const WindowId origin_window = application.window_id();
    REQUIRE(application.open_file(source.path().string()).has_value());

    const WorkbenchId foreground_workbench = application.create_workbench("notes");
    const BufferId foreground_buffer = application.buffer_id();
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }

    CHECK(application.workbench_id() == foreground_workbench);
    CHECK(application.buffer_id() == foreground_buffer);
    const std::optional<BufferId> opened =
        application.buffer_id_by_resource(source.path().string());
    REQUIRE(opened.has_value());
    const std::vector<BufferId> foreground = application.workbench_buffers(foreground_workbench);
    CHECK(std::ranges::find(foreground, *opened) == foreground.end());

    REQUIRE(application.switch_workbench(origin_workbench));
    CHECK(application.window_id() == origin_window);
    CHECK(application.buffer_id() == *opened);
    CHECK(application.buffer_resource(application.session().buffer().id()) ==
          std::optional<std::string>{source.path().string()});
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
    CHECK(application.buffer_name(application.session().buffer().id()) == "*Help*");
    const std::string help = application.session().snapshot().content().to_string();
    CHECK(help.find("C-x C-s  file.save") != std::string::npos);
    CHECK(help.find("C-x C-c  application.quit") != std::string::npos);

    send_keys(application, "M-x");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).provider == "commands");
    REQUIRE(application.interaction().state()->candidates.size() > 2);
    CHECK(application.input_focus() == "minibuffer");
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
    CHECK(application.interaction_selection().value() == 1);
    CHECK(application.last_command() == "interaction.next-candidate");
    send_keys(application, "C-p");
    CHECK(application.interaction_selection().value() == 0);
    send_keys(application, "Down");
    CHECK(application.interaction_selection().value() == 1);
    send_keys(application, "Up");
    CHECK(application.interaction_selection().value() == 0);

    application.insert_text("buffer next");
    REQUIRE_FALSE(application.interaction().state()->candidates.empty());
    CHECK(application.interaction().state()->candidates.front().value == "buffer.next");
    CHECK(application.interaction_selection().value() == 0);
    send_keys(application, "C-g");
    CHECK_FALSE(application.interaction().active());
    CHECK(application.input_focus() == "window");
    CHECK(application.runtime()
              .keymaps()
              .definition(application.active_keymap_layers().back().keymap)
              .name == "application.global");
}

TEST_CASE("minibuffer history keys navigate named Scheme interaction history") {
    EditorApplication application = make_application("sample.cc", "alpha beta alpha");

    send_keys(application, "C-s");
    application.insert_text("alpha");
    send_keys(application, "RET");
    CHECK_FALSE(application.interaction().active());

    send_keys(application, "C-s");
    application.insert_text("draft");
    send_keys(application, "M-p");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().input_text() == "alpha");
    CHECK(application.last_command() == "interaction.previous-history");
    const BufferId minibuffer = application.interaction().state()->buffer;
    const std::expected<GuileMinibufferHistoryState, std::string> previous =
        application.minibuffer_history_state(minibuffer, "search");
    REQUIRE(previous.has_value());
    CHECK(previous->entries == 1);
    CHECK(previous->index == 0);
    CHECK(previous->draft == "draft");
    send_keys(application, "M-n");
    CHECK(application.interaction().input_text() == "draft");
    CHECK(application.last_command() == "interaction.next-history");
    const std::expected<GuileMinibufferHistoryState, std::string> next =
        application.minibuffer_history_state(minibuffer, "search");
    REQUIRE(next.has_value());
    CHECK_FALSE(next->index.has_value());
    CHECK(next->draft == "draft");
    send_keys(application, "M-p");
    application.insert_text("x");
    const std::expected<GuileMinibufferHistoryState, std::string> edited =
        application.minibuffer_history_state(minibuffer, "search");
    REQUIRE(edited.has_value());
    CHECK_FALSE(edited->index.has_value());
    CHECK(edited->draft.empty());
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
    application.insert_text("👩‍💻");
    send_keys(application, "Backspace");
    CHECK(application.interaction().input_text().empty());
    application.insert_text("abc");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().input_text() == "abc");
    CHECK(application.interaction().input_caret() == TextOffset{3});

    send_keys(application, "C-b C-b");
    CHECK(application.interaction().input_caret() == TextOffset{1});
    application.insert_text("X");
    CHECK(application.interaction().input_text() == "aXbc");
    CHECK(application.interaction().input_caret() == TextOffset{2});
    send_keys(application, "C-f");
    CHECK(application.interaction().input_caret() == TextOffset{3});
    send_keys(application, "C-a C-d");
    CHECK(application.interaction().input_text() == "Xbc");
    CHECK(application.interaction().input_caret() == TextOffset{0});
    send_keys(application, "C-/");
    CHECK(application.interaction().input_text() == "aXbc");
    CHECK(application.last_command() == "edit.undo");
    send_keys(application, "C-M-/");
    CHECK(application.interaction().input_text() == "Xbc");
    CHECK(application.last_command() == "edit.redo");
    send_keys(application, "C-e");
    CHECK(application.interaction().input_caret() == TextOffset{3});
    CHECK(application.session().caret() == document_caret);

    send_keys(application, "C-g");
}

TEST_CASE("document and minibuffer share the ordinary kill ring") {
    EditorApplication application = make_application("sample.cc", "alpha beta");
    application.session().set_selection({.anchor = TextOffset{0}, .head = TextOffset{5}});

    send_keys(application, "M-w");
    send_keys(application, "M-x");
    REQUIRE(application.interaction().active());
    send_keys(application, "C-y");
    CHECK(application.interaction().input_text() == "alpha");
    CHECK(application.session().snapshot().content().to_string() == "alpha beta");
    send_keys(application, "C-g");
}

TEST_CASE("Scheme expression evaluation uses the shared minibuffer command loop") {
    EditorApplication application = make_application("sample.scm", "source\n");
    const BufferId source = application.buffer_id();

    send_keys(application, "M-:");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(interaction_policy(application).prompt == "Eval: ");
    CHECK(interaction_policy(application).history == "scheme-expression");
    application.insert_text("(+ 1 2)");
    send_keys(application, "RET");

    CHECK_FALSE(application.interaction().active());
    CHECK(application.buffer_id() == source);
    CHECK(application.message() == "3");
    CHECK(application.last_command() == "scheme.eval-expression.accept");
}

TEST_CASE("Scheme buffer and region evaluation share a persistent user module") {
    const std::string source_text =
        "(define buffer-value 9)\n(display \"loaded\\n\")\n(* buffer-value 2)\n";
    EditorApplication application = make_application("sample.scm", source_text);
    EditorRuntime& runtime = application.runtime();
    const BufferId source = application.buffer_id();
    const CommandId eval_buffer = require_command(runtime, "scheme.eval-buffer");
    CommandContext buffer_context(runtime, application.window_id(), source, application.view_id());

    const CommandResult evaluated = runtime.commands().invoke(eval_buffer, buffer_context);
    REQUIRE(evaluated.has_value());
    const BufferId result = application.buffer_id();
    CHECK(result != source);
    const Buffer& result_buffer = runtime.buffers().get(result);
    CHECK(application.buffer_name(result_buffer.id()) == "*Scheme Evaluation*");
    CHECK(application.buffer_kind(result_buffer.id()) == BufferKind::Generated);
    CHECK(result_buffer.read_only());
    CHECK(application.style_origin() == "scheme evaluation");
    const std::string result_text = result_buffer.snapshot().content().to_string();
    CHECK(result_text.find("Output:\nloaded\n") != std::string::npos);
    CHECK(result_text.find("Values:\n  18\n") != std::string::npos);

    REQUIRE(application.switch_buffer(source));
    const auto region_start = static_cast<std::uint32_t>(source_text.find("(* buffer-value 2)"));
    application.session().set_selection(
        {.anchor = TextOffset{region_start}, .head = TextOffset{region_start + 18}});
    CommandContext region_context(runtime, application.window_id(), source, application.view_id());
    const CommandResult region =
        runtime.commands().invoke(require_command(runtime, "scheme.eval-region"), region_context);
    REQUIRE(region.has_value());
    CHECK(application.buffer_id() == source);
    CHECK(application.message() == "18");

    const CommandResult expression = runtime.commands().invoke(
        require_command(runtime, "scheme.eval-expression.accept"), region_context,
        CommandInvocation{.arguments = {std::string("(+ buffer-value 1)")}, .prefix = {}});
    REQUIRE(expression.has_value());
    CHECK(application.message() == "10");
    CHECK(application.buffer_count() == 2);
}

TEST_CASE("Scheme REPL is an editor buffer with persistent evaluation and input history") {
    EditorApplication application = make_application("sample.scm", "");
    EditorRuntime& runtime = application.runtime();

    send_keys(application, "C-c C-z");
    CHECK(application.last_command() == "scheme.repl");
    const Buffer& repl = runtime.buffers().get(application.buffer_id());
    CHECK(application.buffer_name(repl.id()) == "*Scheme REPL*");
    CHECK(application.buffer_kind(repl.id()) == BufferKind::Process);
    CHECK_FALSE(repl.read_only());
    const std::optional<ModeId> major = repl.modes().major();
    REQUIRE(major.has_value());
    CHECK(runtime.modes().definition(*major).name == "scheme-repl-mode");
    CHECK(repl.editable_start() == std::optional(repl.snapshot().content().end_offset()));

    const std::string initial_transcript = repl.snapshot().content().to_string();
    send_keys(application, "Backspace");
    CHECK(repl.snapshot().content().to_string() == initial_transcript);
    application.insert_text("x");
    send_keys(application, "Backspace");
    CHECK(repl.snapshot().content().to_string() == initial_transcript);

    application.insert_text("(define repl-value 40)");
    send_keys(application, "RET");
    application.insert_text("(+ repl-value 2)");
    send_keys(application, "RET");
    std::string transcript = application.session().snapshot().content().to_string();
    CHECK(transcript.find("(define repl-value 40)\n") != std::string::npos);
    CHECK(transcript.find("(+ repl-value 2)\n=> 42\nscheme> ") != std::string::npos);
    CHECK(repl.editable_start() == std::optional(repl.snapshot().content().end_offset()));
    send_keys(application, "Backspace");
    CHECK(application.session().snapshot().content().to_string() == transcript);

    application.session().set_caret(TextOffset{});
    application.insert_text(";");
    CHECK(application.session().snapshot().content().to_string() == transcript);
    application.session().set_caret(application.session().snapshot().content().end_offset());
    send_keys(application, "M-p");
    transcript = application.session().snapshot().content().to_string();
    CHECK(transcript.ends_with("scheme> (+ repl-value 2)"));
    send_keys(application, "M-p");
    transcript = application.session().snapshot().content().to_string();
    CHECK(transcript.ends_with("scheme> (define repl-value 40)"));
    send_keys(application, "M-n");
    transcript = application.session().snapshot().content().to_string();
    CHECK(transcript.ends_with("scheme> (+ repl-value 2)"));
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

TEST_CASE("focused interactions inherit and remap text editing commands") {
    EditorApplication application = make_application("sample.cc", "document");

    send_keys(application, "M-x");
    application.insert_text("foo bar");
    send_keys(application, "C-a");
    send_keys(application, "M-f");
    REQUIRE(application.interaction().state() != nullptr);
    CHECK(application.interaction().input_caret() == TextOffset{3});
    CHECK(application.last_command() == "cursor.forward-word");

    send_keys(application, "C-k");
    CHECK(application.interaction().input_text() == "foo");
    CHECK(application.last_command() == "edit.kill-line");
    send_keys(application, "C-y");
    CHECK(application.interaction().input_text() == "foo bar");
    CHECK(application.session().snapshot().content().to_string() == "document");
}

TEST_CASE("kill line policy follows the active language facets") {
    EditorApplication structural = make_application("sample.cc", "int x = f(a);");
    structural.session().set_caret(TextOffset{11});
    send_keys(structural, "C-k");
    CHECK(structural.session().snapshot().content().to_string() == "int x = f(a);");

    EditorApplication plain = make_application({}, "int x = f(a);");
    plain.session().set_caret(TextOffset{11});
    send_keys(plain, "C-k");
    CHECK(plain.session().snapshot().content().to_string() == "int x = f(a");
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
    const std::optional<KeymapId> default_keymap = runtime.keymaps().find("editor.default");
    REQUIRE(default_keymap);
    runtime.keymaps().bind(*default_keymap, "C-z", global.command);
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
    CHECK(application.active_keymap_layers().front().scope == "minibuffer");
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
    std::filesystem::create_directories(directory / ".git", ignored);
    {
        std::ofstream head(directory / ".git" / "HEAD", std::ios::binary);
        head << "ref: refs/heads/main\n";
    }
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
    application.runtime().resource_policies().define_project_provider("late-test-provider",
                                                                      {".git"});
    while (application.has_background_work()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    const BufferId second = application.buffer_id();
    CHECK(second != first);
    const std::optional<ProjectId> project_id =
        application.buffer_project(application.session().buffer().id());
    REQUIRE(project_id.has_value());
    const Project& project = application.runtime().projects().get(*project_id);
    CHECK(project.discovery_provider() == "cind.vcs");
    CHECK(project.discovery_marker() == ".git");
    application.insert_text("B");
    application.session().view().viewport().left_column = 3;

    const CommandId switch_buffer =
        application.runtime().commands().find("buffer.switch.accept").value_or(CommandId{});
    REQUIRE(switch_buffer);
    CommandContext switch_context(application.runtime(), application.window_id(),
                                  application.buffer_id(), application.view_id());
    const CommandResult switched = application.runtime().commands().invoke(
        switch_buffer, switch_context,
        CommandInvocation{.arguments = {application.buffer_name(first)}, .prefix = {}});
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

    CommandContext kill_last_context(application.runtime(), application.window_id(),
                                     application.buffer_id(), application.view_id());
    REQUIRE(application.runtime().commands().invoke(force_kill, kill_last_context).has_value());
    CHECK(application.buffer_count() == 1);
    CHECK(application.buffer_name(application.session().buffer().id()) == "*scratch*");
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
    CHECK(application.buffer_name(help_buffer.id()) == "*Help*");
    CHECK(application.buffer_kind(help_buffer.id()) == BufferKind::Generated);
    CHECK(help_buffer.read_only());
    const ModeId help_mode = help_buffer.modes().major().value_or(ModeId{});
    REQUIRE(help_mode);
    CHECK(runtime.modes().definition(help_mode).name == "special-mode");
    CHECK(application.style_origin() == "help");
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

TEST_CASE("completion commands use the completion-active keymap and apply buffer words") {
    EditorApplication application = make_application("sample.cc", "foo foobar fo");
    application.session().set_caret(TextOffset{13});

    send_keys(application, "C-M-i");
    REQUIRE(application.completion().active());
    REQUIRE(application.completion().state() != nullptr);
    REQUIRE(application.completion().state()->matches.size() == 2);
    const auto word_provider = std::ranges::find_if(
        application.completion().state()->providers, [](const CompletionProviderState& provider) {
            return provider.provider.kind == CompletionProviderKind::Scripted &&
                   std::ranges::any_of(provider.items, [](const CompletionItem& item) {
                       return item.kind == "word" && item.detail == "buffer";
                   });
        });
    REQUIRE(word_provider != application.completion().state()->providers.end());
    REQUIRE(word_provider->items.size() == 2);
    CHECK(std::ranges::all_of(word_provider->items, [](const CompletionItem& item) {
        return item.kind == "word" && item.detail == "buffer";
    }));
    CHECK(std::ranges::any_of(application.active_keymap_layers(), [](const KeymapLayer& layer) {
        return layer.scope == "completion-active";
    }));

    send_keys(application, "C-n");
    CHECK(application.completion_selection() == 1);
    send_keys(application, "RET");
    CHECK_FALSE(application.completion().active());
    const std::string text = application.session().snapshot().content().to_string();
    CHECK((text == "foo foobar foo" || text == "foo foobar foobar"));
}

TEST_CASE("Scheme buffer completion uses Ares bindings and Scheme symbol ranges") {
    EditorApplication application = make_application("sample.scm", "(cind-live-bin");
    EditorRuntime& runtime = application.runtime();
    CommandContext context(runtime, application.window_id(), application.buffer_id(),
                           application.view_id());
    const CommandResult evaluated = runtime.commands().invoke(
        require_command(runtime, "scheme.eval-expression.accept"), context,
        CommandInvocation{.arguments = {"(begin (define (cind-live-binding value) value) 42)"},
                          .prefix = {}});
    REQUIRE(evaluated.has_value());
    application.session().set_caret(TextOffset{14});

    send_keys(application, "C-M-i");
    const CompletionState* state = application.completion().state();
    REQUIRE(state != nullptr);
    const auto provider =
        std::ranges::find_if(state->providers, [](const CompletionProviderState& candidate) {
            return candidate.provider.kind == CompletionProviderKind::Scripted &&
                   std::ranges::any_of(candidate.items, [](const CompletionItem& item) {
                       return item.label == "cind-live-binding";
                   });
        });
    REQUIRE(provider != state->providers.end());
    CAPTURE(provider->error);
    CHECK(provider->error.empty());
    CHECK(std::ranges::any_of(provider->items, [](const CompletionItem& item) {
        return item.label == "cind-live-binding";
    }));
    const auto match = std::ranges::find_if(state->matches, [](const CompletionMatch& candidate) {
        return candidate.item.provider.kind == CompletionProviderKind::Scripted &&
               candidate.item.label == "cind-live-binding";
    });
    REQUIRE(match != state->matches.end());
    REQUIRE(match->item.edit.has_value());
    CHECK(match->item.edit->insert_range == make_range(1, 14));
    CHECK(match->item.kind == "function");

    const std::size_t selected = static_cast<std::size_t>(match - state->matches.begin());
    for (std::size_t index = 0; index < selected; ++index) {
        send_keys(application, "C-n");
    }
    send_keys(application, "RET");
    CHECK(application.session().snapshot().content().to_string() == "(cind-live-binding");
}

TEST_CASE("Scheme typing starts Ares completion and deletion refilters cached bindings") {
    EditorApplication application = make_application("sample.scm", "(");
    application.session().set_caret(TextOffset{1});

    application.insert_text("d");
    const CompletionState* state = application.completion().state();
    REQUIRE(state != nullptr);
    CHECK(state->request.trigger.kind == CompletionTriggerKind::Automatic);
    CHECK(state->request.query == "d");
    const auto define_match =
        std::ranges::find_if(state->matches, [](const CompletionMatch& candidate) {
            return candidate.item.label == "define";
        });
    REQUIRE(define_match != state->matches.end());
    const std::uint64_t define_id = define_match->item.id;
    const auto scripted =
        std::ranges::find_if(state->providers, [](const CompletionProviderState& candidate) {
            return candidate.provider.kind == CompletionProviderKind::Scripted &&
                   std::ranges::any_of(candidate.items, [](const CompletionItem& item) {
                       return item.label == "define";
                   });
        });
    REQUIRE(scripted != state->providers.end());
    CHECK_FALSE(scripted->is_incomplete);
    const auto unresolved =
        std::ranges::find_if(scripted->items, [](const CompletionItem& candidate) {
            return candidate.label != "define" && !candidate.resolved;
        });
    REQUIRE(unresolved != scripted->items.end());
    CHECK(unresolved->documentation.empty());

    REQUIRE(application.focus_completion(
        static_cast<std::size_t>(define_match - state->matches.begin())));
    state = application.completion().state();
    REQUIRE(state != nullptr);
    const auto resolved_define =
        std::ranges::find_if(state->matches, [define_id](const CompletionMatch& candidate) {
            return candidate.item.id == define_id;
        });
    REQUIRE(resolved_define != state->matches.end());
    CHECK(resolved_define->item.resolved);

    application.insert_text("e");
    state = application.completion().state();
    REQUIRE(state != nullptr);
    CHECK(state->request.query == "de");
    CHECK(std::ranges::any_of(state->matches, [define_id](const CompletionMatch& candidate) {
        return candidate.item.label == "define" && candidate.item.id == define_id;
    }));

    send_keys(application, "Backspace");
    state = application.completion().state();
    REQUIRE(state != nullptr);
    CHECK(state->request.query == "d");
    CHECK(std::ranges::any_of(state->matches, [define_id](const CompletionMatch& candidate) {
        return candidate.item.label == "define" && candidate.item.id == define_id;
    }));

    send_keys(application, "Backspace");
    CHECK_FALSE(application.completion().active());
    CHECK(application.session().snapshot().content().to_string() == "(");
}

TEST_CASE("include completion selects the asynchronous path provider") {
    TemporaryDirectory directory("cind-completion-path-test");
    const std::filesystem::path source = directory.write("main.cc", "#include \"fo");
    (void)directory.write("foo.hpp", "");
    WakeSignal wake;
    EditorApplication application =
        make_application(source.string(), "#include \"fo",
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    application.session().set_caret(TextOffset{12});

    send_keys(application, "C-M-i");
    REQUIRE(application.completion().state() != nullptr);
    REQUIRE(application.completion().state()->providers.size() == 1);
    CHECK(application.completion().state()->providers.front().provider.kind ==
          CompletionProviderKind::Scripted);
    CHECK(application.completion().state()->providers.front().pending);
    REQUIRE(wake.wait());
    CHECK(application.poll_background_work());
    REQUIRE(application.completion().state() != nullptr);
    REQUIRE(application.completion().state()->matches.size() == 1);
    CHECK(application.completion().state()->matches.front().item.label == "foo.hpp");
    CHECK(application.completion().state()->matches.front().item.kind == "file");
    CHECK(application.completion().state()->matches.front().item.detail == "path");
}

TEST_CASE("Scheme completion policy owns syntax gating and UTF-8 byte anchors") {
    EditorApplication comment = make_application("comment.cc", "// fo");
    comment.session().set_caret(TextOffset{5});

    send_keys(comment, "C-M-i");
    CHECK_FALSE(comment.completion().active());
    CHECK(comment.message() == "completion is suppressed in the current syntax context");

    EditorApplication unicode = make_application("unicode.cc", "😀foo fo");
    unicode.session().set_caret(TextOffset{10});

    send_keys(unicode, "C-M-i");
    const CompletionState* state = unicode.completion().state();
    REQUIRE(state != nullptr);
    CHECK(state->request.anchor == TextOffset{8});
    CHECK(state->request.caret == TextOffset{10});
    CHECK(state->request.query == "fo");
}

TEST_CASE("LSP navigation feeds location lists and the workbench jump graph") {
    TemporaryDirectory commands("cind-lsp-navigation-path");
    std::filesystem::create_symlink(CIND_LSP_TEST_SERVER, commands.path() / "clangd");
    ScopedPath path_scope(commands.path());
    TemporaryDirectory project("cind-lsp-navigation-project");
    const std::filesystem::path source = project.write("main.cc", "a😀bc\n");
    const std::filesystem::path definition = project.write("main.cc.definition", "a😀bc\n");
    WakeSignal wake;
    EditorApplication application =
        make_application(source.string(), "a😀bc\n",
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    const BufferId origin = application.buffer_id();
    application.session().set_caret(TextOffset{5});

    REQUIRE(application.execute_command("lsp.references"));
    for (int iteration = 0;
         application.location_lists(application.workbench_id()).empty() && iteration < 20;
         ++iteration) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    const std::vector<LocationListSnapshot> lists =
        application.location_lists(application.workbench_id());
    REQUIRE(lists.size() == 1);
    CHECK(lists.front().source == "references");
    CHECK(lists.front().item_count == 2);
    REQUIRE(lists.front().materialized_buffer.has_value());
    CHECK(application.buffer_id() == *lists.front().materialized_buffer);
    const std::vector<OpenBufferSnapshot> buffers = application.open_buffers();
    const auto location_buffer =
        std::ranges::find_if(buffers, [&](const OpenBufferSnapshot& buffer) {
            return buffer.buffer == *lists.front().materialized_buffer;
        });
    REQUIRE(location_buffer != buffers.end());
    CHECK(location_buffer->major_mode == "cind.location-list");

    send_keys(application, "RET");
    for (int iteration = 0; application.path() != "/tmp/first.cpp" && iteration < 20; ++iteration) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    CHECK(application.path() == "/tmp/first.cpp");
    CHECK(std::ranges::any_of(application.jump_graphs().front().edges,
                              [](const JumpEdge& edge) { return edge.kind == "list"; }));

    REQUIRE(application.switch_buffer(origin));
    const std::vector<Diagnostic> diagnostics =
        application.runtime().buffers().get(origin).diagnostics();
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].range == make_range(1, 5));
    CHECK(diagnostics[0].severity == DiagnosticSeverity::Warning);
    CHECK(diagnostics[0].source == "cind-test");
    REQUIRE(application.execute_command("diagnostic.list"));
    CHECK(application.buffer_kind(application.session().buffer().id()) == BufferKind::Process);
    CHECK(application.session().buffer().locations().size() == 1);
    send_keys(application, "RET");
    CHECK(application.buffer_id() == origin);
    CHECK(application.session().caret() == TextOffset{1});

    application.session().set_caret(TextOffset{});
    REQUIRE(application.execute_command("lsp.definition"));
    for (int iteration = 0; application.session().caret() != TextOffset{5} && iteration < 20;
         ++iteration) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    CHECK(application.path() == definition.string());
    CHECK(application.session().caret() == TextOffset{5});
    CHECK(std::ranges::any_of(application.jump_graphs().front().edges,
                              [](const JumpEdge& edge) { return edge.kind == "def"; }));
}

TEST_CASE("C++ completion merges clangd semantic candidates with local providers") {
    if (!std::filesystem::exists("/usr/bin/clangd")) {
        return;
    }
    TemporaryDirectory directory("cind-lsp-completion-test");
    const std::string source =
        "struct Foo { int semantic_member; }; int main() { Foo value; value.semantic_ }\n";
    const std::filesystem::path path = directory.write("main.cc", source);
    WakeSignal wake;
    EditorApplication application =
        make_application(path.string(), source,
                         {.write_clipboard = {}, .read_clipboard = {}, .wake_event_loop = [&wake] {
                              wake.notify();
                          }});
    application.session().set_caret(
        TextOffset{static_cast<std::uint32_t>(source.rfind("semantic_") + 9)});

    send_keys(application, "C-M-i");
    REQUIRE(application.completion().state() != nullptr);
    const auto lsp_pending = [&] {
        const CompletionState* state = application.completion().state();
        if (state == nullptr) {
            return false;
        }
        const auto provider = std::ranges::find_if(state->providers, [](const auto& candidate) {
            return candidate.provider.kind == CompletionProviderKind::Lsp;
        });
        return provider != state->providers.end() && provider->pending;
    };
    while (lsp_pending()) {
        REQUIRE(wake.wait());
        (void)application.poll_background_work();
    }
    REQUIRE(application.completion().state() != nullptr);
    CHECK(std::ranges::any_of(
        application.completion().state()->matches, [](const CompletionMatch& match) {
            return match.item.provider.kind == CompletionProviderKind::Lsp &&
                   match.item.label.find("semantic_member") != std::string::npos;
        }));
}
