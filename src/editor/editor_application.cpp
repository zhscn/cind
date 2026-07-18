#include "editor/editor_application.hpp"

#include "cli/style_loader.hpp"
#include "commands/file_io.hpp"
#include "editor/noun_evaluator.hpp"
#include "project/project_files.hpp"
#include "project/search_results.hpp"
#include "syntax/structure.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

namespace fs = std::filesystem;

std::expected<std::string, std::string> normalized_path(std::string_view input) {
    if (input.empty()) {
        return std::unexpected("file path is empty");
    }
    std::error_code error;
    fs::path path = fs::absolute(fs::path(input), error).lexically_normal();
    if (error) {
        return std::unexpected(std::format("invalid path: {}", error.message()));
    }
    return path.string();
}

TextRange plain_kill_line_range(const Text& text, TextOffset caret) {
    const std::uint32_t line = text.position(caret).line;
    const TextOffset content_end = text.line_content_end(line);
    if (caret < content_end) {
        return {caret, content_end};
    }
    const TextOffset line_end = text.line_range(line).end;
    return caret < line_end ? TextRange{caret, line_end} : TextRange{caret, caret};
}

struct LoadedFileData {
    std::string resource;
    std::string contents;
    CppIndentStyle style;
    std::string style_origin;
    ModeId mode;
    std::optional<ProjectDiscovery> project;
};

} // namespace

EditorApplication::EditorApplication(EditorApplicationSpec spec)
    : guile_(
          runtime_,
          {.display_buffer = [this](WindowId window,
                                    BufferId buffer) -> std::expected<void, std::string> {
               try {
                   if (!show_buffer(window, buffer)) {
                       return std::unexpected("buffer cannot be displayed");
                   }
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .display_generated_buffer =
               [this](WindowId window, std::string name, std::string text) {
                   return display_generated_buffer(window, std::move(name), std::move(text));
               },
           .move_caret_to_line =
               [this](ViewId view, std::uint32_t line, std::uint32_t display_column) {
                   return move_caret_to_line(view, line, display_column);
               },
           .undo = [this](ViewId view) { return editing_mechanisms_.undo(view); },
           .redo = [this](ViewId view) { return editing_mechanisms_.redo(view); },
           .set_view_caret =
               [this](ViewId view, std::uint32_t offset) {
                   session_for(view).set_caret(TextOffset{offset});
                   reveal_caret_ = true;
               },
           .move_caret_lines =
               [this](ViewId view, std::int64_t delta) {
                   editing_mechanisms_.move_lines(view, delta);
               },
           .move_caret_line_boundary =
               [this](ViewId view, bool end) { editing_mechanisms_.move_line_boundary(view, end); },
           .delete_grapheme =
               [this](ViewId view, bool forward, bool structural) {
                   switch (editing_mechanisms_.delete_grapheme(
                       view, forward,
                       structural ? DeleteGraphemeMode::Structural : DeleteGraphemeMode::Raw)) {
                   case DeleteGraphemeOutcome::Unchanged:
                       return GuileDeleteOutcome::Unchanged;
                   case DeleteGraphemeOutcome::Deleted:
                       return GuileDeleteOutcome::Deleted;
                   case DeleteGraphemeOutcome::MovedOverPair:
                       return GuileDeleteOutcome::MovedOverPair;
                   case DeleteGraphemeOutcome::MovedOverLiteral:
                       return GuileDeleteOutcome::MovedOverLiteral;
                   }
                   throw std::logic_error("unknown delete-grapheme outcome");
               },
           .newline = [this](ViewId view) { editing_mechanisms_.newline(view); },
           .indent = [this](ViewId view) -> std::optional<std::string> {
               const std::optional<FormatRole> role = editing_mechanisms_.indent(view);
               return role ? std::optional(std::string(format_role_name(*role))) : std::nullopt;
           },
           .type_text =
               [this](ViewId view, std::string_view text) {
                   editing_mechanisms_.type_text(view, text);
               },
           .page_rows = [this] { return command_page_rows_; },
           .interaction_status =
               [this] {
                   const InteractionState* state = interaction_.state();
                   return GuileInteractionStatus{
                       .active = state != nullptr,
                       .picker = state != nullptr && state->request.kind == InteractionKind::Picker,
                       .has_history = state != nullptr && !state->request.history.empty()};
               },
           .submit_interaction = [this]() -> std::expected<CommandDispatch, std::string> {
               std::expected<InteractionSubmission, std::string> submission = interaction_.submit();
               if (!submission) {
                   return std::unexpected(std::move(submission.error()));
               }
               interaction_session_.reset();
               return CommandDispatch{.command = submission->accept_command,
                                      .invocation = std::move(submission->invocation),
                                      .target = submission->target};
           },
           .move_interaction_candidate =
               [this](int delta) { return interaction_.move_selection(delta); },
           .move_interaction_history =
               [this](int delta) {
                   return delta < 0 ? interaction_.previous_history() : interaction_.next_history();
               },
           .cancel_interaction =
               [this] {
                   interaction_session_.reset();
                   return interaction_.cancel();
               },
           .cancel_pending_input = [this] { command_loop_.cancel_pending(); },
           .view_position =
               [this](ViewId view) {
                   const EditSession& active = session_for(view);
                   const DocumentSnapshot snapshot = active.snapshot();
                   const Text& text = snapshot.content();
                   const TextOffset caret = active.caret();
                   const LinePosition position = text.position(caret);
                   return GuileViewPosition{
                       .line = position.line,
                       .line_count = text.line_count(),
                       .display_column = static_cast<std::uint32_t>(
                           ui::display_column(text, caret, active.style().tab_width)),
                       .byte = caret.value,
                       .byte_count = text.size_bytes()};
               },
           .location_navigation =
               [this] {
                   const LocationNavigationSnapshot navigation = location_navigation();
                   return GuileLocationNavigation{.buffer = navigation.buffer,
                                                  .selected_index = navigation.selected_index,
                                                  .location_count = navigation.location_count};
               },
           .set_location_navigation =
               [this](std::optional<BufferId> buffer,
                      std::optional<std::size_t> selected) -> std::expected<void, std::string> {
               if (!buffer) {
                   location_navigation_.reset();
                   return {};
               }
               const Buffer* list = runtime_.buffers().try_get(*buffer);
               if (list == nullptr) {
                   return std::unexpected("location list is no longer available");
               }
               if (selected && *selected >= list->locations().size()) {
                   return std::unexpected("location index is out of range");
               }
               location_navigation_ =
                   LocationNavigationState{.buffer = *buffer, .selected_index = selected};
               return {};
           },
           .position_buffer_view = [this](WindowId window, BufferId buffer, std::uint32_t offset)
               -> std::expected<void, std::string> {
               try {
                   if (runtime_.windows().try_get(window) == nullptr ||
                       runtime_.buffers().try_get(buffer) == nullptr) {
                       return std::unexpected("location list is no longer available");
                   }
                   ViewState* view = find_view(window, buffer);
                   if (view == nullptr) {
                       const ViewId created = create_view(window, buffer, TextOffset{offset});
                       view = &view_state_for(created);
                   } else {
                       runtime_.views().set_caret(view->view, TextOffset{offset});
                   }
                   editing_mechanisms_.reset_preferred_column(view->view);
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .open_file_at =
               [this](WindowId window, const std::string& path, std::uint32_t line,
                      std::uint32_t column) {
                   return open_file(path, window,
                                    LinePosition{.line = line, .byte_column = column});
               },
           .set_message = [this](std::string message) { message_ = std::move(message); },
           .ensure_project_index = [this](ProjectId project) -> std::expected<void, std::string> {
               try {
                   const Project& definition = runtime_.projects().get(project);
                   if (definition.index_revision() == 0 && !definition.indexing()) {
                       project_service_->request_index(project);
                   }
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .open_file =
               [this](WindowId window, const std::string& path) {
                   return open_file(path, window, std::nullopt);
               },
           .start_project_search =
               [this](ProjectId project, WindowId window, std::string query) {
                   return start_project_search(project, std::move(query), window);
               },
           .set_buffer_resource = [this](BufferId buffer, const std::string& input)
               -> std::expected<void, std::string> {
               std::expected<std::string, std::string> path = normalized_path(input);
               if (!path) {
                   return std::unexpected(path.error());
               }
               try {
                   runtime_.buffers().set_resource(buffer, *path, BufferKind::File);
                   runtime_.buffers().rename(buffer, fs::path(*path).filename().string());
                   runtime_.buffers().get(buffer).modes().set_major(runtime_.modes(),
                                                                    mode_for_resource(*path));
                   runtime_.projects().assign(buffer, runtime_.projects().find_for_resource(*path));
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .save_buffer = [this](BufferId buffer) { save(buffer); },
           .open_buffers =
               [this] {
                   std::vector<BufferId> result;
                   result.reserve(buffers_.size());
                   for (const std::unique_ptr<BufferState>& state : buffers_) {
                       result.push_back(state->buffer);
                   }
                   return result;
               },
           .create_buffer =
               [this](GuileBufferCreation spec) -> std::expected<BufferId, std::string> {
               try {
                   return create_buffer(BufferSpec{.name = std::move(spec.name),
                                                   .initial_text = std::move(spec.initial_text),
                                                   .kind = spec.kind,
                                                   .resource_uri = std::nullopt,
                                                   .read_only = spec.read_only},
                                        CppIndentStyle{}, std::move(spec.style_origin),
                                        spec.major_mode);
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .buffer_saving =
               [this](BufferId buffer) { return state_for(buffer).pending_save.has_value(); },
           .release_buffer =
               [this](BufferId buffer, BufferId replacement) {
                   return release_buffer(buffer, replacement);
               },
           .request_exit = [this] { quit_ = true; },
           .split_window = [this](WindowId window,
                                  WindowSplitAxis axis) -> std::expected<void, std::string> {
               return split_window(window, axis) ? std::expected<void, std::string>{}
                                                 : std::unexpected("window cannot be split");
           },
           .delete_window = [this](WindowId window) -> std::expected<void, std::string> {
               return delete_window(window) ? std::expected<void, std::string>{}
                                            : std::unexpected("cannot delete the only window");
           },
           .delete_other_windows = [this](WindowId window) -> std::expected<void, std::string> {
               return delete_other_windows(window) ? std::expected<void, std::string>{}
                                                   : std::unexpected("unknown window");
           },
           .open_windows =
               [this] {
                   return std::vector<WindowId>(window_layout_.leaves().begin(),
                                                window_layout_.leaves().end());
               },
           .focus_window = [this](WindowId window) -> std::expected<void, std::string> {
               return focus_window(window) ? std::expected<void, std::string>{}
                                           : std::unexpected("unknown window");
           },
           .request_redraw = [this] { reveal_caret_ = true; },
           .active_key_bindings =
               [this] {
                   std::vector<GuileKeyBindingSummary> result;
                   const auto append = [&](KeymapId keymap) {
                       for (const KeymapBinding& binding : runtime_.keymaps().bindings(keymap)) {
                           const std::string keys = format_key_sequence(binding.sequence);
                           if (std::ranges::any_of(result, [&](const GuileKeyBindingSummary& item) {
                                   return item.keys == keys;
                               })) {
                               continue;
                           }
                           result.push_back(
                               {.keys = keys,
                                .command = runtime_.commands().definition(binding.command).name});
                       }
                   };
                   for (const KeymapId keymap : command_loop_.override_keymaps()) {
                       append(keymap);
                   }
                   for (const KeymapLayer& layer : command_loop_.keymap_layers()) {
                       append(layer.keymap);
                   }
                   return result;
               },
           .active_keymap_layers =
               [this](WindowId window) {
                   std::vector<KeymapId> result;
                   if (window == active_window_) {
                       result.insert(result.end(), command_loop_.override_keymaps().begin(),
                                     command_loop_.override_keymaps().end());
                       for (const KeymapLayer& layer : command_loop_.keymap_layers()) {
                           result.push_back(layer.keymap);
                       }
                       return result;
                   }
                   for (const KeymapLayer& layer : base_keymap_layers(window)) {
                       result.push_back(layer.keymap);
                   }
                   return result;
               },
           .base_keymap_layers =
               [this](WindowId window) {
                   std::vector<KeymapId> result;
                   for (const KeymapLayer& layer : base_keymap_layers(window)) {
                       result.push_back(layer.keymap);
                   }
                   return result;
               },
           .set_selection =
               [this](ViewId view, ViewSelection selection) {
                   session_for(view).set_selection(std::move(selection));
                   reveal_caret_ = true;
               },
           .clear_selection = [this](ViewId view) { session_for(view).clear_selection(); },
           .replace_selection = [this](ViewId view, ViewSelection selection,
                                       std::vector<std::string> replacements)
               -> std::expected<ViewSelection, std::string> {
               try {
                   EditSession& active = session_for(view);
                   const RevisionId before = active.snapshot().revision();
                   ViewSelection result =
                       active.replace_selection(std::move(selection), replacements);
                   if (active.snapshot().revision() != before) {
                       after_edit();
                   }
                   return result;
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .selection_texts = [this](ViewId view, const ViewSelection& selection)
               -> std::expected<std::vector<std::string>, std::string> {
               try {
                   return session_for(view).selection_texts(selection);
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .erase_range = [this](ViewId view,
                                 GuileTextRange range) -> std::expected<void, std::string> {
               try {
                   session_for(view).erase(
                       TextRange{TextOffset{range.start}, TextOffset{range.end}});
                   after_edit();
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .insert_text = [this](ViewId view, std::vector<std::string> replacements)
               -> std::expected<void, std::string> {
               try {
                   EditSession& active = session_for(view);
                   const RevisionId before = active.snapshot().revision();
                   active.insert_text(replacements);
                   if (active.snapshot().revision() != before) {
                       after_edit();
                   }
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .soft_kill_range = [this](ViewId view) -> std::optional<GuileTextRange> {
               EditSession& active = session_for(view);
               const DocumentSnapshot snapshot = active.snapshot();
               const TextRange range =
                   active.has_language_facet(LanguageFacet::StructuralEditing)
                       ? soft_kill_end(active.analysis(LanguageFacet::StructuralEditing).tree,
                                       snapshot.content(), active.caret())
                       : plain_kill_line_range(snapshot.content(), active.caret());
               if (range.empty()) {
                   return std::nullopt;
               }
               return GuileTextRange{range.start.value, range.end.value};
           },
           .thing_selection =
               [this](ViewId view, const ViewSelection& source, std::string_view name,
                      bool bounds) -> std::expected<std::optional<ViewSelection>, std::string> {
               const Buffer& buffer = session_for(view).buffer();
               const EffectiveModePolicy policy = runtime_.modes().effective_policy(buffer.modes());
               std::string definition_name(name);
               if (const auto binding =
                       std::ranges::find_if(policy.things,
                                            [name](const ModeThingBinding& candidate) {
                                                return candidate.name == name;
                                            });
                   binding != policy.things.end()) {
                   definition_name = binding->definition;
               }
               const std::optional<ThingId> thing = runtime_.things().find(definition_name);
               if (!thing) {
                   return std::unexpected(std::format("unknown thing '{}'", definition_name));
               }
               EditSession& active = session_for(view);
               const DocumentSnapshot snapshot = active.snapshot();
               ViewSelection selected = source;
               for (SelectionRange& range : selected.ranges) {
                   const std::optional<ThingMatch> match = evaluate_thing(
                       runtime_.things(), *thing, snapshot,
                       active.analysis(LanguageFacet::StructuralEditing).tree, range.head);
                   if (!match) {
                       return std::optional<ViewSelection>{};
                   }
                   range = bounds ? match->bounds : match->inner;
               }
               selected.metadata = std::format("((thing . {}) (definition . {}) (extent . {}))",
                                               name, definition_name, bounds ? "bounds" : "inner");
               return std::optional<ViewSelection>{std::move(selected)};
           },
           .motion_selection = [this](ViewId view, const ViewSelection& source,
                                      std::string_view name, std::int64_t count,
                                      bool extend) -> std::expected<ViewSelection, std::string> {
               const std::optional<MotionId> motion = runtime_.motions().find(name);
               if (!motion) {
                   return std::unexpected(std::format("unknown motion '{}'", name));
               }
               EditSession& active = session_for(view);
               const MotionMechanism mechanism = runtime_.motions().definition(*motion).mechanism;
               if ((mechanism == MotionMechanism::ForwardExpression ||
                    mechanism == MotionMechanism::BackwardExpression ||
                    mechanism == MotionMechanism::UpList) &&
                   !active.has_language_facet(LanguageFacet::StructuralEditing)) {
                   return std::unexpected("structural motion is unavailable for the current mode");
               }
               const SyntaxTree* tree =
                   active.has_language_facet(LanguageFacet::StructuralEditing)
                       ? &active.analysis(LanguageFacet::StructuralEditing).tree
                       : nullptr;
               return evaluate_motion(runtime_.motions(), *motion, active.snapshot(), tree, source,
                                      count, extend);
           },
           .expand_selection = [this](ViewId view, const ViewSelection& source)
               -> std::expected<std::optional<ViewSelection>, std::string> {
               try {
                   const EditSession& active = session_for(view);
                   if (!active.has_language_facet(LanguageFacet::StructuralEditing)) {
                       return std::optional<ViewSelection>{};
                   }
                   return evaluate_node_expansion(
                       active.analysis(LanguageFacet::StructuralEditing).tree, source);
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .write_clipboard = [this](std::string_view text) -> std::expected<void, std::string> {
               if (!platform_services_.write_clipboard) {
                   return {};
               }
               return platform_services_.write_clipboard(text);
           },
           .read_clipboard = [this]() -> std::expected<std::optional<std::string>, std::string> {
               if (!platform_services_.read_clipboard) {
                   return std::optional<std::string>{};
               }
               std::expected<std::string, std::string> read = platform_services_.read_clipboard();
               if (!read) {
                   return std::unexpected(std::move(read.error()));
               }
               return std::optional<std::string>{std::move(*read)};
           },
           .start_async_task =
               [this](ScriptAsyncRequest request, ScriptAsyncCallbacks callbacks) {
                   return script_async_.start(std::move(request), std::move(callbacks));
               },
           .cancel_async_task = [this](std::uint64_t task) { return script_async_.cancel(task); },
           .async_tasks = [this] { return script_async_.tasks(); }}),
      interaction_(runtime_, runtime_.interaction_providers()),
      editing_mechanisms_(
          [this](ViewId view) -> EditSession& { return session_for(view); },
          {.edited = [this] { after_edit(); }, .caret_moved = [this] { reveal_caret_ = true; }}),
      command_loop_(runtime_), platform_services_(std::move(spec.platform_services)),
      async_runtime_(std::move(platform_services_.wake_event_loop)), script_async_(async_runtime_) {
    interaction_.attach_async_runtime(async_runtime_);
    project_service_ =
        std::make_unique<ProjectService>(runtime_, async_runtime_, [this](ProjectId project) {
            if (InteractionState* active = interaction_.state();
                active != nullptr && active->request.provider == "project-files" &&
                session().buffer().project_id() == project) {
                interaction_.refresh_candidates();
            }
        });
    register_input_states();
    register_modes();
    fundamental_mode_ = runtime_.modes().find("fundamental-mode").value_or(ModeId{});
    if (!fundamental_mode_) {
        throw std::logic_error("Guile mode policy did not define fundamental-mode");
    }
    special_mode_ = runtime_.modes().find("special-mode").value_or(ModeId{});
    if (!special_mode_) {
        throw std::logic_error("Guile mode policy did not define special-mode");
    }
    location_list_mode_ = runtime_.modes().find("cind.location-list").value_or(ModeId{});
    if (!location_list_mode_) {
        throw std::logic_error("Guile mode policy did not define cind.location-list");
    }
    register_resource_policies();
    register_commands();
    register_interaction_providers();
    register_keymaps();
    if (spec.init_file) {
        if (std::expected<void, std::string> loaded = guile_.load_extension(*spec.init_file);
            !loaded) {
            message_ = std::format("init failed: {}", loaded.error());
        }
    }

    const bool deferred_initial_load = !spec.initial_text && !spec.path.empty();
    BufferSpec initial_buffer{
        .name = {},
        .initial_text = spec.initial_text ? std::move(*spec.initial_text) : std::string(),
        .kind = deferred_initial_load || spec.path.empty() ? BufferKind::Scratch : BufferKind::File,
        .resource_uri = std::nullopt,
        .read_only = false};
    if (!spec.path.empty() && !deferred_initial_load) {
        std::expected<std::string, std::string> path = normalized_path(spec.path);
        if (!path) {
            throw std::invalid_argument(path.error());
        }
        initial_buffer.resource_uri = std::move(*path);
    }
    const ModeId initial_mode = initial_buffer.resource_uri
                                    ? mode_for_resource(*initial_buffer.resource_uri)
                                    : fundamental_mode_;
    const BufferId initial = create_buffer(std::move(initial_buffer), spec.style,
                                           std::move(spec.style_origin), initial_mode);
    const ViewId initial_view = create_view({}, initial);
    active_window_ = runtime_.windows().create(initial_view);
    view_state_for(initial_view).window = active_window_;
    window_layout_ = WindowLayout(active_window_);
    if (spec.initial_line > 0 && !deferred_initial_load) {
        apply_position(active_window_, {.line = spec.initial_line - 1, .byte_column = 0});
    }

    sync_keymaps();
    if (deferred_initial_load) {
        startup_placeholder_ = initial;
        if (std::expected<void, std::string> opened = open_file(
                spec.path, active_window_,
                spec.initial_line > 0
                    ? std::optional(LinePosition{.line = spec.initial_line - 1, .byte_column = 0})
                    : std::nullopt);
            !opened) {
            message_ = std::format("open failed: {}", opened.error());
        }
    }
}

EditorApplication::~EditorApplication() {
    interaction_session_.reset();
    (void)interaction_.cancel();
    guile_.shutdown_async_tasks();
}

BufferId EditorApplication::buffer_id() const {
    return runtime_.views().get(view_id()).buffer_id();
}

BufferId EditorApplication::buffer_id(WindowId window) const {
    return runtime_.views().get(view_id(window)).buffer_id();
}

ViewId EditorApplication::view_id() const {
    return runtime_.windows().get(active_window_).view_id();
}

ViewId EditorApplication::view_id(WindowId window) const {
    return runtime_.windows().get(window).view_id();
}

EditSession& EditorApplication::session() {
    return *active_view().session;
}

const EditSession& EditorApplication::session() const {
    return *active_view().session;
}

EditSession& EditorApplication::session(WindowId window) {
    return session_for(view_id(window));
}

const EditSession& EditorApplication::session(WindowId window) const {
    return session_for(view_id(window));
}

const TokenBuffer& EditorApplication::syntax_tokens() const {
    return syntax_tokens(active_window_);
}

const TokenBuffer& EditorApplication::syntax_tokens(WindowId window) const {
    static const TokenBuffer plain_text;
    const Buffer& buffer = session(window).buffer();
    if (!runtime_.language_provider(buffer.id(), LanguageFacet::Highlighting)) {
        return plain_text;
    }
    return session(window).analysis(LanguageFacet::Highlighting).tree.tokens();
}

void EditorApplication::refresh_default_keymap() {
    std::expected<std::size_t, std::string> installed = guile_.install_default_keymaps();
    if (!installed) {
        throw std::runtime_error(std::format("Guile keymap policy failed: {}", installed.error()));
    }
}

bool EditorApplication::handle_key(KeyStroke key, int page_rows) {
    command_page_rows_ = std::max(1, page_rows);
    last_key_ = format_key_stroke(key);
    const ViewId focused_view = interaction_.active() ? interaction_.state()->view : view_id();
    runtime_.views().clear_input_feedback(focused_view);
    sync_keymaps();
    {
        const View& active_view = runtime_.views().get(focused_view);
        if (const std::optional<InputStateId> state = active_view.input_states().top()) {
            const InputStateRegistry::Definition& definition =
                runtime_.input_states().definition(*state);
            if (definition.handler) {
                CommandContext context = command_context();
                if (std::optional<CommandLoopResult> override =
                        command_loop_.dispatch_override(key, context)) {
                    const bool consumed = handle_loop_result(std::move(*override));
                    sync_keymaps();
                    return consumed;
                }
                InputStateHandlerResult handled = definition.handler(context, key);
                if (!handled) {
                    command_loop_.cancel_pending();
                    message_ = handled.error();
                    sync_keymaps();
                    return true;
                }
                if (handled->kind == InputStateHandlerActionKind::Consume) {
                    command_loop_.cancel_pending();
                    sync_keymaps();
                    return true;
                }
                if (handled->kind == InputStateHandlerActionKind::Dispatch) {
                    if (!handled->command) {
                        command_loop_.cancel_pending();
                        message_ = "input state handler returned an invalid command";
                        sync_keymaps();
                        return true;
                    }
                    if (handled->invocation.prefix.empty()) {
                        handled->invocation.prefix = command_loop_.pending_prefix();
                    }
                    const RevisionId interaction_revision = interaction_.input_revision();
                    const bool consumed = handle_loop_result(
                        command_loop_.execute(handled->command, context, handled->invocation));
                    refresh_interaction_after_edit(interaction_revision);
                    sync_keymaps();
                    return consumed;
                }
                if (handled->kind == InputStateHandlerActionKind::Pending) {
                    if (!handled->feedback) {
                        command_loop_.cancel_pending();
                        message_ = "input state handler returned pending without feedback";
                        sync_keymaps();
                        return true;
                    }
                    runtime_.views().set_input_feedback(active_view.id(), *handled->feedback);
                    command_loop_.cancel_pending();
                    sync_keymaps();
                    return true;
                }
                sync_keymaps();
            }
        }
    }
    const RevisionId interaction_revision = interaction_.input_revision();
    CommandContext context = command_context();
    const CommandPrefix text_prefix = command_loop_.pending_prefix();
    CommandLoopResult result = command_loop_.dispatch(key, context);
    const bool preserve_prefix_for_text = result.status == CommandLoopStatus::NotHandled &&
                                          !result.consumed && !text_prefix.empty() &&
                                          key.code == KeyCode::Character && key.character >= U' ' &&
                                          !has_modifier(key.modifiers, KeyModifier::Control) &&
                                          !has_modifier(key.modifiers, KeyModifier::Alt) &&
                                          !has_modifier(key.modifiers, KeyModifier::Super) &&
                                          text_input_policy() == TextInputPolicy::Accept;
    const bool consumed = handle_loop_result(std::move(result));
    refresh_interaction_after_edit(interaction_revision);
    if (preserve_prefix_for_text) {
        command_loop_.set_pending_prefix(text_prefix);
    }
    sync_keymaps();
    return consumed;
}

bool EditorApplication::execute_command(std::string_view name,
                                        const CommandInvocation& invocation) {
    const std::optional<CommandId> command = runtime_.commands().find(name);
    if (!command) {
        command_loop_.cancel_pending();
        message_ = std::format("unknown command '{}'", name);
        sync_keymaps();
        return false;
    }
    const RevisionId interaction_revision = interaction_.input_revision();
    CommandContext context = command_context();
    const bool consumed = handle_loop_result(command_loop_.execute(*command, context, invocation));
    refresh_interaction_after_edit(interaction_revision);
    sync_keymaps();
    return consumed;
}

TextInputPolicy EditorApplication::text_input_policy() const {
    return input_state().text_input;
}

const InputStateRegistry::Definition& EditorApplication::input_state() const {
    if (const InteractionState* interaction = interaction_.state()) {
        const std::optional<InputStateId> state =
            runtime_.views().get(interaction->view).input_states().top();
        if (!state) {
            throw std::logic_error("focused minibuffer view has no input state");
        }
        return runtime_.input_states().definition(*state);
    }
    return input_state(active_window_);
}

const InputStateRegistry::Definition& EditorApplication::input_state(WindowId window) const {
    const std::optional<InputStateId> state =
        runtime_.views().get(view_id(window)).input_states().top();
    if (!state) {
        throw std::logic_error("view has no input state");
    }
    return runtime_.input_states().definition(*state);
}

void EditorApplication::insert_text(std::string_view text) {
    if (text.empty()) {
        return;
    }
    const InputStateRegistry::Definition& state = input_state();
    if (state.text_input == TextInputPolicy::Ignore) {
        return;
    }
    if (!state.text_command) {
        command_loop_.cancel_pending();
        message_ = std::format("input state '{}' has no text command", state.name);
        last_key_ = "text";
        return;
    }
    const std::optional<CommandId> command = runtime_.commands().find(*state.text_command);
    if (!command) {
        command_loop_.cancel_pending();
        message_ = std::format("unknown input text command '{}'", *state.text_command);
        last_key_ = "text";
        return;
    }
    const RevisionId interaction_revision = interaction_.input_revision();
    CommandContext context = command_context();
    CommandInvocation invocation{.arguments = {std::string(text)},
                                 .prefix = command_loop_.pending_prefix()};
    (void)handle_loop_result(command_loop_.execute(*command, context, invocation));
    last_key_ = "text";
    refresh_interaction_after_edit(interaction_revision);
    sync_keymaps();
}

void EditorApplication::reset_preferred_column() {
    editing_mechanisms_.reset_preferred_column(interaction_.active() ? interaction_.state()->view
                                                                     : view_id());
}

std::expected<void, std::string> EditorApplication::open_file(std::string_view input) {
    return open_file(input, active_window_, std::nullopt);
}

std::expected<void, std::string>
EditorApplication::open_file(std::string_view input, WindowId target_window,
                             std::optional<LinePosition> position) {
    std::expected<std::string, std::string> normalized = normalized_path(input);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }
    if (const std::optional<BufferId> existing = runtime_.buffers().find_by_resource(*normalized)) {
        (void)show_buffer(target_window, *existing);
        if (position) {
            apply_position(target_window, *position);
        }
        return {};
    }
    if (auto pending = std::ranges::find_if(
            pending_opens_, [&](const PendingOpen& open) { return open.resource == *normalized; });
        pending != pending_opens_.end()) {
        pending->target_window = target_window;
        pending->position = position;
        message_ = std::format("opening {}…", *normalized);
        return {};
    }

    const std::string resource = *normalized;
    try {
        pending_opens_.push_back({.resource = resource,
                                  .target_window = target_window,
                                  .position = position,
                                  .task = {}});
        PendingOpen& pending = pending_opens_.back();
        auto requested_resource = std::make_shared<const std::string>(resource);
        const ModeId requested_mode = mode_for_resource(resource);
        const bool load_cpp_style =
            runtime_.language_provider(requested_mode, LanguageFacet::Indentation).has_value();
        auto project_providers = std::make_shared<const std::vector<ProjectDiscoveryProvider>>(
            runtime_.resource_policies().project_providers());
        pending.task = async_runtime_.submit({
            .work = [this, requested_resource, requested_mode, load_cpp_style,
                     project_providers](const std::stop_token& cancellation) -> AsyncCompletion {
                std::expected<FileReadResult, std::error_code> file =
                    read_file_contents(*requested_resource, cancellation);
                if (!file) {
                    if (file.error() == std::make_error_code(std::errc::operation_canceled)) {
                        throw AsyncTaskCancelled();
                    }
                    throw std::system_error(file.error(),
                                            std::format("cannot open {}", *requested_resource));
                }
                auto loaded_data = std::make_shared<LoadedFileData>(LoadedFileData{
                    .resource = *requested_resource,
                    .contents = std::move(file->contents),
                    .style = {},
                    .style_origin = load_cpp_style ? "llvm (fallback)" : "plain text",
                    .mode = requested_mode,
                    .project = std::nullopt,
                });
                if (load_cpp_style) {
                    if (std::optional<LoadedStyle> loaded_style =
                            load_clang_format_style(fs::path(*requested_resource).parent_path())) {
                        loaded_data->style = loaded_style->style;
                        loaded_data->style_origin = loaded_style->config_path.filename().string();
                    }
                }
                std::expected<std::optional<ProjectDiscovery>, std::error_code> project =
                    discover_project(*requested_resource, *project_providers, cancellation);
                if (!project &&
                    project.error() == std::make_error_code(std::errc::operation_canceled)) {
                    throw AsyncTaskCancelled();
                }
                if (project) {
                    loaded_data->project = std::move(*project);
                }
                return [this, loaded_data] {
                    finish_open(std::move(loaded_data->resource), std::move(loaded_data->contents),
                                loaded_data->style, std::move(loaded_data->style_origin),
                                loaded_data->mode, loaded_data->project);
                };
            },
            .cancelled = [this, requested_resource] { cancel_open(*requested_resource); },
            .failed =
                [this, requested_resource](const std::exception_ptr& failure) {
                    fail_open(*requested_resource, failure);
                },
        });
        message_ = std::format("opening {}…", resource);
        return {};
    } catch (const std::exception& exception) {
        std::erase_if(pending_opens_,
                      [&](const PendingOpen& open) { return open.resource == resource; });
        return std::unexpected(exception.what());
    }
}

void EditorApplication::finish_open(std::string resource, std::string contents,
                                    CppIndentStyle style, std::string style_origin, ModeId mode,
                                    const std::optional<ProjectDiscovery>& project) {
    const auto pending = std::ranges::find_if(
        pending_opens_, [&](const PendingOpen& open) { return open.resource == resource; });
    if (pending == pending_opens_.end()) {
        return;
    }
    const WindowId requested_window = pending->target_window;
    const std::optional<LinePosition> position = pending->position;
    pending_opens_.erase(pending);

    try {
        BufferId buffer;
        if (const std::optional<BufferId> existing =
                runtime_.buffers().find_by_resource(resource)) {
            buffer = *existing;
        } else {
            buffer = create_buffer(BufferSpec{.name = {},
                                              .initial_text = std::move(contents),
                                              .kind = BufferKind::File,
                                              .resource_uri = resource,
                                              .read_only = false},
                                   style, std::move(style_origin), mode);
        }
        project_service_->attach_buffer(buffer, project);
        const WindowId target = runtime_.windows().try_get(requested_window) != nullptr
                                    ? requested_window
                                    : active_window_;
        if (runtime_.windows().try_get(target) != nullptr) {
            (void)show_buffer(target, buffer);
            if (position) {
                apply_position(target, *position);
            }
        }
        if (startup_placeholder_ && *startup_placeholder_ != buffer) {
            const BufferId placeholder = *startup_placeholder_;
            startup_placeholder_.reset();
            if (Buffer* startup = runtime_.buffers().try_get(placeholder);
                startup != nullptr && !startup->modified()) {
                std::expected<void, std::string> removed = release_buffer(placeholder, buffer);
                if (!removed) {
                    startup_placeholder_ = placeholder;
                }
            }
        }
        message_ = std::format("opened {}", resource);
    } catch (const std::exception& exception) {
        message_ = std::format("open failed: {}", exception.what());
    }
}

std::expected<void, std::string> EditorApplication::start_project_search(ProjectId project,
                                                                         std::string query,
                                                                         WindowId target_window) {
    const Project* definition = runtime_.projects().try_get(project);
    if (definition == nullptr || definition->roots().empty()) {
        return std::unexpected("project has no root");
    }
    if (project_search_.process.valid()) {
        (void)async_runtime_.terminate(project_search_.process);
    }
    if (project_search_.parse_task.valid()) {
        (void)async_runtime_.cancel(project_search_.parse_task);
    }
    const std::uint64_t generation = ++project_search_.generation;
    const std::string root = definition->roots().front();
    try {
        project_search_.process = async_runtime_.spawn({
            .file = "rg",
            .arguments = {"--line-number", "--column", "--no-heading", "--color", "never",
                          "--smart-case", "--null", "--", query, "."},
            .working_directory = root,
            .completed =
                [this, project, target_window, generation,
                 query = std::move(query)](AsyncProcessResult result) mutable {
                    finish_project_search(project, target_window, generation, std::move(query),
                                          std::move(result));
                },
            .cancelled = [this, generation] { cancel_project_search(generation); },
            .failed =
                [this, generation](const std::exception_ptr& failure) {
                    fail_project_search(generation, failure);
                },
        });
        message_ = std::format("searching project {}…", definition->name());
        return {};
    } catch (const std::exception& exception) {
        project_search_.process = {};
        return std::unexpected(exception.what());
    }
}

void EditorApplication::finish_project_search(ProjectId project, WindowId target_window,
                                              std::uint64_t generation, std::string query,
                                              AsyncProcessResult result) {
    if (project_search_.generation != generation || project_search_.process != result.process) {
        return;
    }
    project_search_.process = {};
    if (result.exit_status > 1 || result.term_signal != 0) {
        std::string detail = std::move(result.standard_error);
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) {
            detail.pop_back();
        }
        message_ = detail.empty()
                       ? std::format("project search failed with status {}", result.exit_status)
                       : std::format("project search failed: {}", detail);
        return;
    }
    try {
        const Project* definition = runtime_.projects().try_get(project);
        if (definition == nullptr || definition->roots().empty()) {
            message_ = "project search failed: project is no longer available";
            return;
        }
        struct ParseJob {
            ProjectId project;
            WindowId target_window;
            std::uint64_t generation = 0;
            std::string query;
            std::string root;
            std::string output;
            EditorApplication* application = nullptr;
        };
        auto job = std::make_shared<ParseJob>(ParseJob{.project = project,
                                                       .target_window = target_window,
                                                       .generation = generation,
                                                       .query = std::move(query),
                                                       .root = definition->roots().front(),
                                                       .output = std::move(result.standard_output),
                                                       .application = this});
        project_search_.parse_task = async_runtime_.submit({
            .work = [job](const std::stop_token& cancellation) -> AsyncCompletion {
                if (cancellation.stop_requested()) {
                    throw AsyncTaskCancelled();
                }
                std::expected<LocationListDocument, std::string> document =
                    parse_rg_search_results({.project_root = job->root, .output = job->output});
                if (!document) {
                    throw std::runtime_error(document.error());
                }
                if (document->text.empty()) {
                    document->text = std::format("No matches for: {}\n", job->query);
                }
                if (cancellation.stop_requested()) {
                    throw AsyncTaskCancelled();
                }
                auto shared_document = std::make_shared<LocationListDocument>(std::move(*document));
                return [job, shared_document] {
                    job->application->finish_project_search_document(
                        job->project, job->target_window, job->generation, std::move(job->query),
                        std::move(*shared_document));
                };
            },
            .cancelled = [this, generation] { cancel_project_search(generation); },
            .failed =
                [this, generation](const std::exception_ptr& failure) {
                    fail_project_search(generation, failure);
                },
        });
        message_ = "preparing project search results…";
    } catch (const std::exception& exception) {
        project_search_.parse_task = {};
        message_ = std::format("project search failed: {}", exception.what());
    }
}

void EditorApplication::finish_project_search_document(ProjectId project, WindowId target_window,
                                                       std::uint64_t generation, std::string query,
                                                       LocationListDocument document) {
    if (project_search_.generation != generation) {
        return;
    }
    project_search_.parse_task = {};
    try {
        const BufferId buffer =
            create_buffer(BufferSpec{.name = std::format("*project grep: {}*", query),
                                     .initial_text = std::move(document.text),
                                     .kind = BufferKind::Process,
                                     .resource_uri = std::nullopt,
                                     .read_only = true},
                          CppIndentStyle{}, "location-list", location_list_mode_);
        runtime_.buffers().set_locations(buffer, std::move(document.locations));
        location_navigation_ =
            LocationNavigationState{.buffer = buffer, .selected_index = std::nullopt};
        runtime_.projects().assign(buffer, project);
        const WindowId target =
            runtime_.windows().try_get(target_window) != nullptr ? target_window : active_window_;
        (void)show_buffer(target, buffer);
        message_ = std::format("project search finished: {}", query);
    } catch (const std::exception& exception) {
        message_ = std::format("project search failed: {}", exception.what());
    }
}

void EditorApplication::fail_project_search(std::uint64_t generation,
                                            const std::exception_ptr& failure) {
    if (project_search_.generation != generation) {
        return;
    }
    project_search_.process = {};
    project_search_.parse_task = {};
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
        message_ = "project search failed";
    } catch (const std::exception& exception) {
        message_ = std::format("project search failed: {}", exception.what());
    } catch (...) {
        message_ = "project search failed";
    }
}

void EditorApplication::cancel_project_search(std::uint64_t generation) {
    if (project_search_.generation != generation) {
        return;
    }
    project_search_.process = {};
    project_search_.parse_task = {};
    message_ = "project search cancelled";
}

void EditorApplication::fail_open(std::string_view resource, const std::exception_ptr& failure) {
    std::erase_if(pending_opens_,
                  [&](const PendingOpen& open) { return open.resource == resource; });
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
        message_ = "open failed";
    } catch (const std::exception& exception) {
        message_ = std::format("open failed: {}", exception.what());
    } catch (...) {
        message_ = "open failed";
    }
}

void EditorApplication::cancel_open(std::string_view resource) {
    std::erase_if(pending_opens_,
                  [&](const PendingOpen& open) { return open.resource == resource; });
    message_ = std::format("open cancelled: {}", resource);
}

bool EditorApplication::switch_buffer(BufferId buffer) {
    return show_buffer(active_window_, buffer);
}

bool EditorApplication::focus_window(WindowId window) {
    if (!window_layout_.contains(window) || runtime_.windows().try_get(window) == nullptr) {
        return false;
    }
    if (window != active_window_) {
        command_loop_.cancel_pending();
    }
    active_window_ = window;
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

bool EditorApplication::split_window(WindowSplitAxis axis) {
    return split_window(active_window_, axis);
}

bool EditorApplication::split_window(WindowId target, WindowSplitAxis axis) {
    if (!window_layout_.contains(target) || runtime_.windows().try_get(target) == nullptr) {
        return false;
    }
    EditSession& source = session(target);
    const ViewportState source_viewport = source.view().viewport();
    const std::optional<ViewSelection> source_selection = source.active_selection();
    const ViewId view = create_view({}, buffer_id(target), source.caret());
    runtime_.views().get(view).viewport() = source_viewport;
    if (source_selection) {
        runtime_.views().set_selection(view, *source_selection);
    }
    const WindowId window = runtime_.windows().create(view);
    view_state_for(view).window = window;
    if (!window_layout_.split({.target = target, .new_window = window, .axis = axis})) {
        destroy_window(window);
        return false;
    }
    reveal_caret_ = true;
    return true;
}

bool EditorApplication::delete_window() {
    return delete_window(active_window_);
}

bool EditorApplication::delete_window(WindowId target) {
    const std::optional<WindowId> replacement = window_layout_.next(target);
    if (!replacement || *replacement == target || !window_layout_.erase(target)) {
        return false;
    }
    if (active_window_ == target) {
        active_window_ = *replacement;
    }
    destroy_window(target);
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

bool EditorApplication::delete_other_windows() {
    return delete_other_windows(active_window_);
}

bool EditorApplication::delete_other_windows(WindowId retained) {
    if (!window_layout_.contains(retained) || runtime_.windows().try_get(retained) == nullptr) {
        return false;
    }
    if (window_layout_.leaves().size() <= 1) {
        return true;
    }
    const std::vector<WindowId> windows(window_layout_.leaves().begin(),
                                        window_layout_.leaves().end());
    (void)window_layout_.retain(retained);
    for (const WindowId window : windows) {
        if (window != retained) {
            destroy_window(window);
        }
    }
    active_window_ = retained;
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

std::expected<void, std::string> EditorApplication::release_buffer(BufferId buffer,
                                                                   BufferId replacement) {
    auto found =
        std::ranges::find_if(buffers_, [buffer](const std::unique_ptr<BufferState>& state) {
            return state->buffer == buffer;
        });
    if (found == buffers_.end()) {
        return std::unexpected("unknown buffer");
    }
    BufferState& target = **found;
    if (target.pending_save) {
        return std::unexpected("buffer has a save in progress");
    }
    if (buffer == replacement || runtime_.buffers().try_get(replacement) == nullptr ||
        std::ranges::none_of(buffers_, [replacement](const std::unique_ptr<BufferState>& state) {
            return state->buffer == replacement;
        })) {
        return std::unexpected("replacement buffer is not open");
    }

    if (location_navigation_ && location_navigation_->buffer == buffer) {
        location_navigation_.reset();
    }

    std::vector<WindowId> affected_windows;
    for (const std::unique_ptr<ViewState>& view : views_) {
        if (view->buffer == buffer) {
            affected_windows.push_back(view->window);
        }
    }
    std::ranges::sort(affected_windows);
    const auto unique_end = std::ranges::unique(affected_windows).begin();
    affected_windows.erase(unique_end, affected_windows.end());
    for (const WindowId window : affected_windows) {
        if (window && runtime_.windows().try_get(window) != nullptr &&
            runtime_.views().get(runtime_.windows().get(window).view_id()).buffer_id() == buffer) {
            (void)show_buffer(window, replacement);
        }
    }

    for (auto it = views_.begin(); it != views_.end();) {
        if ((*it)->buffer != buffer) {
            ++it;
            continue;
        }
        const ViewId view = (*it)->view;
        if (!runtime_.views().erase(view)) {
            throw std::logic_error("view lifecycle registry is inconsistent");
        }
        it = views_.erase(it);
    }
    buffers_.erase(found);
    if (!runtime_.buffers().erase(buffer)) {
        throw std::logic_error("buffer lifecycle registry is inconsistent");
    }
    reveal_caret_ = true;
    sync_keymaps();
    return {};
}

std::vector<OpenBufferSnapshot> EditorApplication::open_buffers() const {
    std::vector<OpenBufferSnapshot> result;
    result.reserve(buffers_.size());
    for (const std::unique_ptr<BufferState>& entry : buffers_) {
        const BufferState& state = *entry;
        const Buffer& buffer = runtime_.buffers().get(state.buffer);
        const ViewState* view = find_view(active_window_, state.buffer);
        const std::optional<ModeId> major = buffer.modes().major();
        const EffectiveModePolicy mode_policy = runtime_.modes().effective_policy(buffer.modes());
        result.push_back(
            {.buffer = state.buffer,
             .view = view != nullptr ? std::optional(view->view) : std::nullopt,
             .name = buffer.name(),
             .resource = buffer.resource_uri(),
             .modified = buffer.modified(),
             .active = state.buffer == buffer_id(),
             .saving = state.pending_save.has_value(),
             .major_mode = major ? runtime_.modes().definition(*major).name : std::string(),
             .interaction_class = mode_policy.interaction_class == InteractionClass::Editing
                                      ? "editing"
                                      : "interface",
             .initial_input_state =
                 [&] {
                     const std::optional<InputStateId> input_state =
                         view != nullptr ? runtime_.views().get(view->view).input_states().base()
                                         : mode_policy.initial_state;
                     return input_state ? runtime_.input_states().definition(*input_state).name
                                        : std::string();
                 }(),
             .things = mode_policy.things,
             .location_count = buffer.locations().size()});
    }
    return result;
}

std::vector<OpenWindowSnapshot> EditorApplication::open_windows() const {
    std::vector<OpenWindowSnapshot> result;
    result.reserve(window_layout_.leaves().size());
    for (const WindowId window : window_layout_.leaves()) {
        const ViewId view = runtime_.windows().get(window).view_id();
        result.push_back({.window = window,
                          .view = view,
                          .buffer = runtime_.views().get(view).buffer_id(),
                          .active = window == active_window_});
    }
    return result;
}

LocationNavigationSnapshot EditorApplication::location_navigation() const {
    if (!location_navigation_) {
        return {};
    }
    const Buffer* buffer = runtime_.buffers().try_get(location_navigation_->buffer);
    if (buffer == nullptr) {
        return {};
    }
    const std::size_t location_count = buffer->locations().size();
    std::optional<std::size_t> selected_index = location_navigation_->selected_index;
    if (selected_index && *selected_index >= location_count) {
        selected_index.reset();
    }
    return {.buffer = location_navigation_->buffer,
            .selected_index = selected_index,
            .location_count = location_count};
}

const InputFeedback* EditorApplication::active_input_feedback() const {
    if (interaction_.active()) {
        return nullptr;
    }
    const std::optional<InputFeedback>& feedback =
        runtime_.views().get(view_id()).input_states().feedback();
    return feedback ? &*feedback : nullptr;
}

std::string EditorApplication::pending_key_sequence_text() const {
    if (const InputFeedback* feedback = active_input_feedback()) {
        return feedback->sequence;
    }
    return command_loop_.pending_sequence_text();
}

std::string EditorApplication::pending_prefix_text() const {
    return format_command_prefix(command_loop_.pending_prefix());
}

std::string EditorApplication::pending_command_text() const {
    const std::string prefix = pending_prefix_text();
    std::string keys = pending_key_sequence_text();
    if (prefix.empty()) {
        return keys;
    }
    return keys.empty() ? prefix : prefix + " " + keys;
}

std::string EditorApplication::pending_input_state_name() const {
    if (active_input_feedback() == nullptr) {
        return {};
    }
    const std::optional<InputStateId> state = runtime_.views().get(view_id()).input_states().top();
    return state ? runtime_.input_states().definition(*state).name : std::string();
}

std::vector<KeyBindingHint> EditorApplication::pending_key_hints() const {
    if (const InputFeedback* feedback = active_input_feedback()) {
        return feedback->hints;
    }
    std::vector<KeyBindingHint> result;
    for (const KeymapCompletion& completion : command_loop_.pending_completions()) {
        std::string detail = completion.label;
        if (completion.command) {
            detail = runtime_.commands().definition(*completion.command).name;
        }
        result.push_back({.key = format_key_stroke(completion.key),
                          .detail = std::move(detail),
                          .prefix = completion.prefix});
    }
    return result;
}

PositionHintProviderResult EditorApplication::position_hints(WindowId window) {
    const ViewId view = view_id(window);
    const std::optional<InputStateId> state = runtime_.views().get(view).input_states().top();
    if (!state) {
        return std::vector<PositionHint>{};
    }
    const PositionHintProvider& provider =
        runtime_.input_states().definition(*state).position_hints;
    if (!provider) {
        return std::vector<PositionHint>{};
    }
    ViewState& view_state = view_state_for(view);
    const DocumentSnapshot snapshot = view_state.session->snapshot();
    const ViewSelection selection = view_state.session->selection_model();
    const EffectiveModePolicy mode_policy =
        runtime_.modes().effective_policy(view_state.session->buffer().modes());
    if (view_state.position_hints && view_state.position_hints->input_state == *state &&
        view_state.position_hints->revision == snapshot.revision() &&
        view_state.position_hints->selection == selection &&
        view_state.position_hints->mode_policy == mode_policy) {
        return view_state.position_hints->result;
    }

    CommandContext context(runtime_, window, buffer_id(window), view);
    PositionHintProviderResult result = provider(context);
    if (!result) {
        view_state.position_hints = ViewState::PositionHintCache{.input_state = *state,
                                                                 .revision = snapshot.revision(),
                                                                 .selection = selection,
                                                                 .mode_policy = mode_policy,
                                                                 .result = result};
        return view_state.position_hints->result;
    }
    const std::optional<InputStateId> current_state =
        runtime_.views().get(view).input_states().top();
    const DocumentSnapshot current_snapshot = view_state.session->snapshot();
    const ViewSelection current_selection = view_state.session->selection_model();
    const EffectiveModePolicy current_mode_policy =
        runtime_.modes().effective_policy(view_state.session->buffer().modes());
    if (current_state != state || current_snapshot.revision() != snapshot.revision() ||
        current_selection != selection || current_mode_policy != mode_policy) {
        return std::unexpected("position hint provider mutated its query context");
    }

    const std::uint32_t document_bytes = snapshot.content().size_bytes();
    std::optional<std::string> validation_error;
    for (const PositionHint& hint : *result) {
        if (hint.position.value > document_bytes) {
            validation_error = std::format("position hint at byte {} is past document end {}",
                                           hint.position.value, document_bytes);
            break;
        }
        if (hint.label.empty()) {
            validation_error = "position hint label must not be empty";
            break;
        }
    }
    if (validation_error) {
        result = std::unexpected(std::move(*validation_error));
    }
    view_state.position_hints = ViewState::PositionHintCache{.input_state = *state,
                                                             .revision = snapshot.revision(),
                                                             .selection = selection,
                                                             .mode_policy = mode_policy,
                                                             .result = std::move(result)};
    return view_state.position_hints->result;
}

const std::string& EditorApplication::path() const {
    const Buffer& buffer = session().buffer();
    return buffer.resource_uri() ? *buffer.resource_uri() : buffer.name();
}

const std::string& EditorApplication::path(WindowId window) const {
    const Buffer& buffer = session(window).buffer();
    return buffer.resource_uri() ? *buffer.resource_uri() : buffer.name();
}

const std::string& EditorApplication::style_origin() const {
    return active_buffer().style_origin;
}

const std::string& EditorApplication::style_origin(WindowId window) const {
    return state_for(buffer_id(window)).style_origin;
}

std::uint32_t EditorApplication::save_generation() const {
    return active_buffer().save_generation;
}

std::uint32_t EditorApplication::save_generation(WindowId window) const {
    return state_for(buffer_id(window)).save_generation;
}

bool EditorApplication::has_background_work() const {
    return async_runtime_.has_work();
}

bool EditorApplication::poll_background_work() {
    return async_runtime_.drain() != 0;
}

void EditorApplication::mark_saved(Text content) {
    mark_saved(buffer_id(), std::move(content));
}

EditorApplication::BufferState& EditorApplication::active_buffer() {
    return state_for(buffer_id());
}

const EditorApplication::BufferState& EditorApplication::active_buffer() const {
    return const_cast<EditorApplication*>(this)->active_buffer();
}

EditorApplication::BufferState& EditorApplication::state_for(BufferId buffer) {
    const auto found =
        std::ranges::find_if(buffers_, [buffer](const std::unique_ptr<BufferState>& state) {
            return state->buffer == buffer;
        });
    if (found == buffers_.end()) {
        throw std::out_of_range("buffer is not open in this application");
    }
    return **found;
}

const EditorApplication::BufferState& EditorApplication::state_for(BufferId buffer) const {
    return const_cast<EditorApplication*>(this)->state_for(buffer);
}

EditorApplication::ViewState& EditorApplication::active_view() {
    return view_state_for(view_id());
}

const EditorApplication::ViewState& EditorApplication::active_view() const {
    return const_cast<EditorApplication*>(this)->active_view();
}

EditorApplication::ViewState& EditorApplication::view_state_for(ViewId view) {
    const auto found = std::ranges::find_if(
        views_, [view](const std::unique_ptr<ViewState>& state) { return state->view == view; });
    if (found == views_.end()) {
        throw std::out_of_range("view has no editor session");
    }
    return **found;
}

const EditorApplication::ViewState& EditorApplication::view_state_for(ViewId view) const {
    return const_cast<EditorApplication*>(this)->view_state_for(view);
}

EditorApplication::ViewState* EditorApplication::find_view(WindowId window, BufferId buffer) {
    const auto found = std::ranges::find_if(views_, [&](const std::unique_ptr<ViewState>& state) {
        return state->window == window && state->buffer == buffer;
    });
    return found == views_.end() ? nullptr : found->get();
}

const EditorApplication::ViewState* EditorApplication::find_view(WindowId window,
                                                                 BufferId buffer) const {
    return const_cast<EditorApplication*>(this)->find_view(window, buffer);
}

EditSession& EditorApplication::session_for(ViewId view) {
    if (interaction_session_ && interaction_session_->view_id() == view) {
        return *interaction_session_;
    }
    return *view_state_for(view).session;
}

const EditSession& EditorApplication::session_for(ViewId view) const {
    return const_cast<EditorApplication*>(this)->session_for(view);
}

BufferId EditorApplication::create_buffer(BufferSpec spec, CppIndentStyle style,
                                          std::string style_origin,
                                          std::optional<ModeId> major_mode, TextOffset caret) {
    (void)caret;
    const BufferId buffer = runtime_.buffers().create(std::move(spec));
    try {
        runtime_.buffers().get(buffer).modes().set_major(runtime_.modes(), major_mode);
        auto state = std::make_unique<BufferState>();
        state->buffer = buffer;
        state->style = std::make_shared<CppIndentStyle>(style);
        state->style_origin = std::move(style_origin);
        buffers_.push_back(std::move(state));
    } catch (...) {
        (void)runtime_.buffers().erase(buffer);
        throw;
    }
    return buffer;
}

ViewId EditorApplication::create_view(WindowId window, BufferId buffer, TextOffset caret) {
    BufferState& buffer_state = state_for(buffer);
    const ViewId view = runtime_.views().create(buffer, caret);
    try {
        if (runtime_.views().get(view).input_states().empty()) {
            throw std::logic_error("mode policy did not derive an input state for the view");
        }
        auto state = std::make_unique<ViewState>();
        state->window = window;
        state->buffer = buffer;
        state->view = view;
        state->session = std::make_unique<EditSession>(runtime_, buffer, view, buffer_state.style);
        views_.push_back(std::move(state));
    } catch (...) {
        (void)runtime_.views().erase(view);
        throw;
    }
    return view;
}

void EditorApplication::register_input_states() {
    const std::expected<std::size_t, std::string> installed = guile_.install_input_states();
    if (!installed) {
        throw std::runtime_error(
            std::format("Guile input state policy failed: {}", installed.error()));
    }
    const std::optional<InputStrategyId> strategy = runtime_.input_strategies().default_strategy();
    if (!strategy) {
        throw std::logic_error("Guile input policy did not define a default input strategy");
    }
    (void)runtime_.input_states().definition(
        runtime_.input_strategies().state(*strategy, InteractionClass::Editing));
    (void)runtime_.input_states().definition(
        runtime_.input_strategies().state(*strategy, InteractionClass::Interface));
}

void EditorApplication::register_modes() {
    const std::expected<std::size_t, std::string> installed = guile_.install_core_modes();
    if (!installed) {
        throw std::runtime_error(std::format("Guile mode policy failed: {}", installed.error()));
    }
}

void EditorApplication::register_resource_policies() {
    const std::expected<std::size_t, std::string> installed =
        guile_.install_core_resource_policies();
    if (!installed) {
        throw std::runtime_error(
            std::format("Guile resource policy failed: {}", installed.error()));
    }
}

bool EditorApplication::show_buffer(WindowId window, BufferId buffer) {
    if (runtime_.windows().try_get(window) == nullptr ||
        runtime_.buffers().try_get(buffer) == nullptr) {
        return false;
    }
    ViewState* view = find_view(window, buffer);
    if (view == nullptr) {
        const ViewId created = create_view(window, buffer);
        view = &view_state_for(created);
    }
    runtime_.windows().set_view(window, view->view);
    reveal_caret_ = true;
    if (window == active_window_) {
        sync_keymaps();
    }
    return true;
}

std::expected<void, std::string>
EditorApplication::display_generated_buffer(WindowId window, std::string name, std::string text) {
    try {
        BufferId buffer;
        if (const std::optional<BufferId> existing = runtime_.buffers().find_by_name(name)) {
            buffer = *existing;
            Buffer& target = runtime_.buffers().get(buffer);
            if (target.kind() != BufferKind::Generated) {
                return std::unexpected(std::format("buffer '{}' is not generated", name));
            }
            target.set_read_only(false);
            try {
                EditTransaction transaction = target.begin_transaction();
                transaction.replace(
                    TextRange{TextOffset{}, target.snapshot().content().end_offset()}, text);
                (void)transaction.commit();
                target.mark_saved(target.snapshot().content());
                target.set_read_only(true);
            } catch (...) {
                target.set_read_only(true);
                throw;
            }
        } else {
            buffer = create_buffer(BufferSpec{.name = std::move(name),
                                              .initial_text = std::move(text),
                                              .kind = BufferKind::Generated,
                                              .resource_uri = std::nullopt,
                                              .read_only = true},
                                   CppIndentStyle{}, "generated", special_mode_);
        }
        if (!show_buffer(window, buffer)) {
            return std::unexpected("generated buffer cannot be displayed");
        }
        ViewState* view = find_view(window, buffer);
        if (view == nullptr) {
            return std::unexpected("generated buffer view was not created");
        }
        runtime_.views().clear_selection(view->view);
        runtime_.views().set_caret(view->view, TextOffset{});
        runtime_.views().get(view->view).viewport() = {};
        editing_mechanisms_.reset_preferred_column(view->view);
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

std::expected<void, std::string>
EditorApplication::move_caret_to_line(ViewId view, std::uint32_t line,
                                      std::uint32_t display_column) {
    try {
        EditSession& target = session_for(view);
        const DocumentSnapshot snapshot = target.snapshot();
        const std::uint32_t target_line = std::min(line, snapshot.content().line_count() - 1);
        target.set_caret(ui::offset_at_display_column(
            snapshot.content(),
            {.line = target_line,
             .column = static_cast<int>(std::min<std::uint32_t>(
                 display_column, static_cast<std::uint32_t>(std::numeric_limits<int>::max())))},
            target.style().tab_width));
        editing_mechanisms_.reset_preferred_column(view);
        reveal_caret_ = true;
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

void EditorApplication::apply_position(WindowId window, LinePosition position) {
    if (runtime_.windows().try_get(window) == nullptr) {
        return;
    }
    EditSession& target = session(window);
    const DocumentSnapshot snapshot = target.snapshot();
    const Text& text = snapshot.content();
    position.line = std::min(position.line, text.line_count() - 1);
    position.byte_column =
        std::min(position.byte_column, text.line_content_range(position.line).length());
    target.set_caret(text.offset(position));
    editing_mechanisms_.reset_preferred_column(target.view_id());
    reveal_caret_ = true;
}

void EditorApplication::destroy_window(WindowId window) {
    if (!runtime_.windows().erase(window)) {
        throw std::logic_error("window lifecycle registry is inconsistent");
    }
    for (auto iterator = views_.begin(); iterator != views_.end();) {
        if ((*iterator)->window != window) {
            ++iterator;
            continue;
        }
        const ViewId view = (*iterator)->view;
        if (!runtime_.views().erase(view)) {
            throw std::logic_error("view lifecycle registry is inconsistent");
        }
        iterator = views_.erase(iterator);
    }
}

BufferId EditorApplication::create_scratch_buffer() {
    return create_buffer(BufferSpec{.name = "*scratch*",
                                    .initial_text = {},
                                    .kind = BufferKind::Scratch,
                                    .resource_uri = std::nullopt,
                                    .read_only = false},
                         CppIndentStyle{}, "llvm (fallback)", fundamental_mode_);
}

ModeId EditorApplication::mode_for_resource(std::string_view resource) const {
    return runtime_.resource_policies().mode_for(resource).value_or(fundamental_mode_);
}

void EditorApplication::register_commands() {
    std::expected<std::size_t, std::string> installed = guile_.install_core_commands();
    if (!installed) {
        throw std::runtime_error(std::format("Guile command policy failed: {}", installed.error()));
    }
}

void EditorApplication::register_interaction_providers() {
    std::expected<std::size_t, std::string> installed = guile_.install_core_providers();
    if (!installed) {
        throw std::runtime_error(
            std::format("Guile provider policy failed: {}", installed.error()));
    }
}

void EditorApplication::register_keymaps() {
    refresh_default_keymap();
    const std::optional<KeymapId> editor_keymap = runtime_.keymaps().find("editor.default");
    const std::optional<KeymapId> application_keymap =
        runtime_.keymaps().find("application.global");
    const std::optional<KeymapId> system_keymap = runtime_.keymaps().find("editor.system");
    if (!editor_keymap || !application_keymap || !system_keymap) {
        throw std::logic_error("Guile keymap policy did not define its root keymaps");
    }
    keymap_ = *editor_keymap;
    application_keymap_ = *application_keymap;
    command_loop_.set_override_keymaps({*system_keymap});
}

std::vector<KeymapLayer> EditorApplication::base_keymap_layers(WindowId window_id) const {
    std::vector<KeymapLayer> layers;
    const auto append = [&](std::span<const KeymapId> maps, std::string_view scope) {
        for (const KeymapId map : maps) {
            if (std::ranges::any_of(
                    layers, [map](const KeymapLayer& layer) { return layer.keymap == map; })) {
                continue;
            }
            layers.push_back({.keymap = map, .scope = std::string(scope)});
        }
    };

    const Window& window = runtime_.windows().get(window_id);
    const View& view = runtime_.views().get(window.view_id());
    const Buffer& buffer = runtime_.buffers().get(view.buffer_id());
    append(window.keymaps(), "window");
    append(view.keymaps(), buffer.kind() == BufferKind::Minibuffer ? "minibuffer" : "view");
    append(buffer.keymaps(), "buffer");
    for (auto mode = buffer.modes().minors().rbegin(); mode != buffer.modes().minors().rend();
         ++mode) {
        const ModeRegistry::Definition& definition = runtime_.modes().definition(*mode);
        append(runtime_.modes().effective_keymaps(*mode),
               std::format("minor-mode:{}", definition.name));
    }
    if (buffer.modes().major()) {
        const ModeRegistry::Definition& definition =
            runtime_.modes().definition(*buffer.modes().major());
        append(runtime_.modes().effective_keymaps(*buffer.modes().major()),
               std::format("major-mode:{}", definition.name));
    }
    if (buffer.kind() != BufferKind::Minibuffer) {
        append(std::span(&keymap_, 1), "editor");
    }
    append(std::span(&application_keymap_, 1), "global");
    return layers;
}

std::vector<KeymapLayer> EditorApplication::window_keymap_layers() const {
    std::vector<KeymapLayer> layers;
    const auto append = [&](std::span<const KeymapLayer> candidates) {
        for (const KeymapLayer& candidate : candidates) {
            if (std::ranges::none_of(layers, [&](const KeymapLayer& layer) {
                    return layer.keymap == candidate.keymap;
                })) {
                layers.push_back(candidate);
            }
        }
    };
    const WindowId focused_window =
        interaction_.active() ? interaction_.state()->window : active_window_;
    const View& view = runtime_.views().get(view_id(focused_window));
    const std::vector<InputStateId>& input_states = view.input_states().stack();
    for (auto state = input_states.rbegin(); state != input_states.rend(); ++state) {
        const InputStateRegistry::Definition& definition =
            runtime_.input_states().definition(*state);
        for (const KeymapId keymap : definition.keymaps) {
            const KeymapLayer layer{
                .keymap = keymap,
                .scope = std::format("input-state:{}{}", definition.name,
                                     state + 1 == input_states.rend() ? "" : ":transient")};
            append(std::span(&layer, 1));
        }
    }
    const std::vector<KeymapLayer> base = base_keymap_layers(focused_window);
    append(base);
    return layers;
}

void EditorApplication::sync_keymaps() {
    std::vector<KeymapLayer> layers = window_keymap_layers();
    const std::span<const KeymapLayer> active = command_loop_.keymap_layers();
    const bool changed =
        layers.size() != active.size() ||
        !std::ranges::equal(layers, active, [](const KeymapLayer& left, const KeymapLayer& right) {
            return left.keymap == right.keymap && left.scope == right.scope;
        });
    if (!changed) {
        return;
    }
    command_loop_.set_keymap_layers(std::move(layers));
}

bool EditorApplication::handle_loop_result(CommandLoopResult result) {
    if (result.command) {
        last_command_ = runtime_.commands().definition(*result.command).name;
    }
    if (result.interaction) {
        CommandContext context = command_context();
        std::expected<void, std::string> started =
            interaction_.start(std::move(*result.interaction), context);
        if (!started) {
            message_ = started.error();
        } else {
            const InteractionState& state = *interaction_.state();
            interaction_session_ =
                std::make_unique<EditSession>(runtime_, state.buffer, state.view, CppIndentStyle{});
            message_.clear();
        }
    } else if (result.status == CommandLoopStatus::Error ||
               result.status == CommandLoopStatus::Disabled ||
               result.status == CommandLoopStatus::Cancelled ||
               (result.status == CommandLoopStatus::NotHandled && result.consumed)) {
        // A prefix does not echo as a message: the pending sequence has its
        // own display channels (which-key title, echo right edge).
        message_ = result.message;
    }
    return result.consumed;
}

CommandContext EditorApplication::command_context() {
    if (const InteractionState* interaction = interaction_.state()) {
        return CommandContext(runtime_, interaction->window, interaction->buffer,
                              interaction->view);
    }
    return CommandContext(runtime_, active_window_, buffer_id(), view_id());
}

void EditorApplication::refresh_interaction_after_edit(RevisionId before) {
    if (interaction_.active() && interaction_.input_revision() != before) {
        interaction_.refresh_candidates();
    }
}

void EditorApplication::after_edit() {
    message_.clear();
    reveal_caret_ = true;
}

void EditorApplication::save(BufferId buffer) {
    BufferState& state = state_for(buffer);
    if (state.pending_save) {
        message_ = "save already in progress";
        return;
    }
    const std::optional<std::string>& resource =
        runtime_.buffers().get(state.buffer).resource_uri();
    if (!resource) {
        message_ = "buffer has no file path";
        return;
    }
    const DocumentSnapshot snapshot = runtime_.buffers().get(state.buffer).snapshot();
    Text content = snapshot.content();
    std::string target_path = *resource;
    try {
        state.pending_save.emplace(PendingSave{.content = content, .task = {}});
        state.pending_save->task = async_runtime_.submit({
            .work = [this, buffer, path = std::move(target_path), content = std::move(content)](
                        const std::stop_token& cancellation) -> AsyncCompletion {
                std::error_code error;
                try {
                    error = save_file_atomically(path, content, cancellation);
                } catch (const std::system_error& exception) {
                    error = exception.code();
                } catch (const std::bad_alloc&) {
                    error = std::make_error_code(std::errc::not_enough_memory);
                } catch (...) {
                    error = std::make_error_code(std::errc::io_error);
                }
                if (error == std::make_error_code(std::errc::operation_canceled)) {
                    throw AsyncTaskCancelled();
                }
                return [this, buffer, error] { finish_save(buffer, error); };
            },
            .cancelled =
                [this, buffer] {
                    BufferState& cancelled = state_for(buffer);
                    cancelled.pending_save.reset();
                    message_ = "save cancelled";
                },
            .failed =
                [this, buffer](const std::exception_ptr& exception) {
                    std::error_code error = std::make_error_code(std::errc::io_error);
                    try {
                        if (exception) {
                            std::rethrow_exception(exception);
                        }
                    } catch (const std::system_error& system) {
                        error = system.code();
                    } catch (const std::bad_alloc&) {
                        error = std::make_error_code(std::errc::not_enough_memory);
                    } catch (...) {
                        error = std::make_error_code(std::errc::io_error);
                    }
                    finish_save(buffer, error);
                },
        });
        message_ = std::format("saving {}…", *resource);
    } catch (const std::system_error& exception) {
        state.pending_save.reset();
        message_ = std::format("save failed: {}", exception.code().message());
    } catch (const std::bad_alloc&) {
        state.pending_save.reset();
        message_ = "save failed: not enough memory";
    } catch (const std::exception& exception) {
        state.pending_save.reset();
        message_ = std::format("save failed: {}", exception.what());
    }
}

void EditorApplication::finish_save(BufferId buffer_id, std::error_code error) {
    BufferState& state = state_for(buffer_id);
    if (!state.pending_save) {
        return;
    }
    const Buffer& buffer = runtime_.buffers().get(buffer_id);
    const std::string display = buffer.resource_uri().value_or(buffer.name());
    if (error) {
        message_ = std::format("save failed: {}", error.message());
    } else {
        mark_saved(buffer_id, std::move(state.pending_save->content));
        message_ = buffer.modified() ? std::format("saved {} · newer edits remain", display)
                                     : std::format("saved {}", display);
    }
    state.pending_save.reset();
}

void EditorApplication::mark_saved(BufferId buffer, Text content) {
    BufferState& state = state_for(buffer);
    runtime_.buffers().get(buffer).mark_saved(std::move(content));
    ++state.save_generation;
}

} // namespace cind
