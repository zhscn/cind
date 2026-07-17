#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/command_loop.hpp"
#include "editor/runtime.hpp"
#include "script/guile_runtime.hpp"

#include <algorithm>
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

} // namespace

TEST_CASE("bundled Guile policy installs available default key bindings") {
    EditorRuntime runtime;
    const CommandId save = define_command(runtime, "file.save");
    const CommandId replace = define_command(runtime, "search.replace");
    const CommandId quit = define_command(runtime, "application.quit");

    GuileRuntime guile(runtime);
    const std::expected<std::size_t, std::string> installed = guile.install_default_keymaps();

    REQUIRE(installed.has_value());
    CHECK(*installed == 4);
    const KeymapId editor = require_keymap(runtime, "editor.default");
    const KeymapId application = require_keymap(runtime, "application.global");
    const KeymapId control_x = require_keymap(runtime, "editor.control-x");
    CHECK(resolve_command(runtime, editor, "C-x C-s") == save);
    CHECK(resolve_command(runtime, editor, "M-%") == replace);
    CHECK(resolve_command(runtime, application, "C-x C-c") == quit);
    const std::vector<KeymapCompletion> root = runtime.keymaps().completions(editor, {});
    const auto prefix = std::ranges::find_if(root, [](const KeymapCompletion& completion) {
        return format_key_stroke(completion.key) == "C-x";
    });
    REQUIRE(prefix != root.end());
    CHECK(prefix->prefix_keymap == control_x);
    CHECK(prefix->label == "C-x");

    const GuileRuntimeSnapshot snapshot = guile.snapshot();
    CHECK(snapshot.engine == "guile");
    CHECK_FALSE(snapshot.version.empty());
    CHECK(snapshot.modules == std::vector<std::string>{"cind command", "cind input", "cind emacs",
                                                       "cind toy-modal", "cind meow", "cind vim",
                                                       "cind helix", "cind structural",
                                                       "cind core"});
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
    CHECK(*first == 16);
    CHECK(*second == 16);
    const InputStateId emacs = runtime.input_states().find("emacs").value_or(InputStateId{});
    REQUIRE(emacs);
    const InputStateRegistry::Definition& definition = runtime.input_states().definition(emacs);
    CHECK(definition.keymaps.empty());
    CHECK(definition.text_input == TextInputPolicy::Accept);
    CHECK(definition.cursor == CursorShape::Beam);
    CHECK_FALSE(definition.handler);
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
    const InputStateId numeric =
        runtime.input_states().find("meow-numeric").value_or(InputStateId{});
    REQUIRE(numeric);
    CHECK(runtime.input_states().definition(numeric).handler);
    const InputStateId read_key =
        runtime.input_states().find("input.read-key").value_or(InputStateId{});
    REQUIRE(read_key);
    CHECK(runtime.input_states().definition(read_key).handler);
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
    CHECK(guile.snapshot().scripted_input_states == 16);
    CHECK(guile.snapshot().scripted_input_strategies == 5);
    const InputStrategyId helix =
        runtime.input_strategies().find("helix").value_or(InputStrategyId{});
    const InputStrategyId vim = runtime.input_strategies().find("vim").value_or(InputStrategyId{});
    const InputStateId structural =
        runtime.input_states().find("structural-node").value_or(InputStateId{});
    const InputStrategyId emacs_strategy =
        runtime.input_strategies().find("emacs").value_or(InputStrategyId{});
    const InputStrategyId toy_strategy =
        runtime.input_strategies().find("toy-modal").value_or(InputStrategyId{});
    REQUIRE(emacs_strategy);
    REQUIRE(toy_strategy);
    REQUIRE(vim);
    REQUIRE(helix);
    REQUIRE(structural);
    CHECK(runtime.input_states().definition(structural).indicator == "NODE");
    CHECK(runtime.input_states().definition(structural).text_input == TextInputPolicy::Ignore);
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
    CHECK(*first == 3);
    CHECK(*second == 3);
    const ModeId fundamental = runtime.modes().find("fundamental-mode").value_or(ModeId{});
    const ModeId prog = runtime.modes().find("prog-mode").value_or(ModeId{});
    const ModeId special = runtime.modes().find("special-mode").value_or(ModeId{});
    REQUIRE(fundamental);
    REQUIRE(prog);
    REQUIRE(special);
    CHECK(runtime.modes().definition(prog).parent == fundamental);
    CHECK(runtime.modes().definition(prog).things ==
          std::vector<ModeThingBinding>{{.name = "angle", .definition = "cind.angle"},
                                        {.name = "defun", .definition = "cind.defun"},
                                        {.name = "word", .definition = "cind.word"},
                                        {.name = "symbol", .definition = "cind.symbol"}});
    CHECK(runtime.things().find("cind.angle").has_value());
    CHECK(runtime.things().find("cind.defun").has_value());
    CHECK(runtime.motions().find("cind.forward-word").has_value());
    CHECK(runtime.motions().find("cind.forward-symbol").has_value());
    CHECK(runtime.modes().definition(special).parent == fundamental);
    CHECK(runtime.modes().definition(special).interaction_class == InteractionClass::Interface);
    const InputStrategyId emacs_strategy =
        runtime.input_strategies().find("emacs").value_or(InputStrategyId{});
    REQUIRE(emacs_strategy);
    CHECK(runtime.input_strategies().default_strategy() == emacs_strategy);
    CHECK(guile.snapshot().mode_revision == 2);
    CHECK(guile.snapshot().scripted_modes == 3);
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
         }});
    const std::expected<std::size_t, std::string> installed = guile.install_core_commands();
    REQUIRE(installed.has_value());
    CHECK(*installed == 133);
    const std::expected<std::size_t, std::string> providers = guile.install_core_providers();
    REQUIRE(providers.has_value());
    CHECK(*providers == 4);
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
    const ProjectId project =
        runtime.projects().create({.name = "sample", .roots = {"/tmp/sample"}});
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
    CHECK(snapshot.scripted_commands == 133);
    CHECK(snapshot.provider_revision == 1);
    CHECK(snapshot.scripted_providers == 4);
    CHECK_FALSE(snapshot.last_error.has_value());
}
