#include "editor/editor_application.hpp"

#include "editor/noun_evaluator.hpp"
#include "editor/resource_policy.hpp"
#include "syntax/structure.hpp"
#include "ui/char_width.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

TextRange plain_kill_line_range(const Text& text, TextOffset caret) {
    const std::uint32_t line = text.position(caret).line;
    const TextOffset content_end = text.line_content_end(line);
    if (caret < content_end) {
        return {caret, content_end};
    }
    const TextOffset line_end = text.line_range(line).end;
    return caret < line_end ? TextRange{caret, line_end} : TextRange{caret, caret};
}

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
               [this](WindowId window, std::string name, std::string text, ModeId mode,
                      std::string style_origin) {
                   return display_generated_buffer(window, std::move(name), std::move(text), mode,
                                                   std::move(style_origin));
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
           .scroll_view_lines = [this](ViewId view,
                                       double lines) { scroll_view_lines(view, lines); },
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
           .type_text = [this](ViewId view,
                               std::string_view text) -> std::expected<void, std::string> {
               if (!session_for(view).has_language_facet(LanguageFacet::StructuralEditing)) {
                   return std::unexpected("structural typing is unavailable for the current mode");
               }
               editing_mechanisms_.type_text(view, text);
               return {};
           },
           .page_rows = [this] { return command_page_rows_; },
           .interaction_status =
               [this] {
                   const InteractionState* state = interaction_.state();
                   return GuileInteractionStatus{
                       .active = state != nullptr,
                       .picker = state != nullptr && state->request.kind == InteractionKind::Picker,
                       .has_history = state != nullptr && !state->request.history.empty(),
                       .history = state != nullptr && !state->request.history.empty()
                                      ? std::optional(state->request.history)
                                      : std::nullopt,
                       .selected = state != nullptr && !state->candidates.empty()
                                       ? std::optional(state->selected)
                                       : std::nullopt,
                       .candidate_count =
                           state != nullptr ? state->candidates.size() : std::size_t{0},
                       .history_index = state != nullptr ? state->history_index : std::nullopt,
                       .history_draft = state != nullptr ? state->history_draft : std::string{}};
               },
           .interaction_provider = [this]() -> std::optional<std::string> {
               const InteractionState* state = interaction_.state();
               return state != nullptr ? std::optional(state->request.provider) : std::nullopt;
           },
           .set_interaction_provider =
               [this](std::string provider) {
                   return interaction_.set_provider(std::move(provider));
               },
           .interaction_origin_project = [this]() -> std::optional<ProjectId> {
               const InteractionState* state = interaction_.state();
               if (state == nullptr) {
                   return std::nullopt;
               }
               const Buffer* buffer = runtime_.buffers().try_get(state->origin.buffer);
               return buffer != nullptr ? buffer->project_id() : std::nullopt;
           },
           .refresh_interaction = [this] { interaction_.refresh_candidates(); },
           .submit_interaction =
               [this]() -> std::expected<GuileInteractionSubmission, std::string> {
               std::expected<InteractionSubmission, std::string> submission = interaction_.submit();
               if (!submission) {
                   return std::unexpected(std::move(submission.error()));
               }
               interaction_session_.reset();
               return GuileInteractionSubmission{
                   .dispatch = {.command = submission->accept_command,
                                .invocation = std::move(submission->invocation),
                                .target = submission->target},
                   .history = std::move(submission->history)};
           },
           .interaction_history =
               [this](std::string_view name) { return interaction_.history(name); },
           .set_interaction_history =
               [this](std::string name, std::vector<std::string> entries) {
                   interaction_.set_history(std::move(name), std::move(entries));
               },
           .select_interaction_candidate =
               [this](std::size_t index) { return interaction_.select(index); },
           .set_interaction_history_position =
               [this](std::optional<std::size_t> index, std::string draft,
                      const std::string& input) {
                   return interaction_.set_history_navigation(index, std::move(draft), input);
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
           .set_message = [this](std::string message) { message_ = std::move(message); },
           .request_project_index = [this](ProjectId project) -> std::expected<void, std::string> {
               try {
                   (void)runtime_.projects().get(project);
                   project_service_->request_index(project);
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .begin_buffer_save = [this](BufferId buffer) { return begin_buffer_save(buffer); },
           .complete_buffer_save = [this](BufferId buffer) { return complete_buffer_save(buffer); },
           .abort_buffer_save = [this](BufferId buffer) { abort_buffer_save(buffer); },
           .open_buffers =
               [this] {
                   std::vector<BufferId> result;
                   result.reserve(buffers_.size());
                   for (const std::unique_ptr<BufferState>& state : buffers_) {
                       result.push_back(state->buffer);
                   }
                   return result;
               },
           .workbenches =
               [this] {
                   std::vector<GuileWorkbenchSummary> result;
                   for (WorkbenchSnapshot snapshot : workbench_snapshots()) {
                       result.push_back({.workbench = snapshot.workbench,
                                         .name = std::move(snapshot.name),
                                         .scope = std::move(snapshot.scope),
                                         .mru = std::move(snapshot.mru),
                                         .active = snapshot.active});
                   }
                   return result;
               },
           .active_workbench = [this] { return workbench_id(); },
           .workbench_buffers = [this](WorkbenchId workbench,
                                       bool widen) { return workbench_buffers(workbench, widen); },
           .create_workbench = [this](std::string name, std::optional<ProjectId> project)
               -> std::expected<WorkbenchId, std::string> {
               try {
                   return create_workbench(std::move(name), project);
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .switch_workbench = [this](WorkbenchId workbench) -> std::expected<void, std::string> {
               return switch_workbench(workbench) ? std::expected<void, std::string>{}
                                                  : std::unexpected("unknown workbench");
           },
           .close_workbench = [this](WorkbenchId workbench) -> std::expected<void, std::string> {
               return close_workbench(workbench)
                          ? std::expected<void, std::string>{}
                          : std::unexpected("cannot close the last workbench");
           },
           .adopt_project = [this](WorkbenchId workbench,
                                   ProjectId project) -> std::expected<void, std::string> {
               const std::vector<WorkbenchSnapshot> snapshots = workbench_snapshots();
               if (std::ranges::none_of(snapshots, [workbench](const WorkbenchSnapshot& snapshot) {
                       return snapshot.workbench == workbench;
                   })) {
                   return std::unexpected("unknown workbench");
               }
               try {
                   (void)adopt_project(workbench, project);
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .expel_buffer = [this](WorkbenchId workbench,
                                  BufferId buffer) -> std::expected<void, std::string> {
               const std::vector<WorkbenchSnapshot> snapshots = workbench_snapshots();
               if (std::ranges::none_of(snapshots, [workbench](const WorkbenchSnapshot& snapshot) {
                       return snapshot.workbench == workbench;
                   })) {
                   return std::unexpected("unknown workbench");
               }
               try {
                   (void)expel_buffer(workbench, buffer);
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
           .create_buffer =
               [this](GuileBufferCreation spec) -> std::expected<BufferId, std::string> {
               try {
                   return create_buffer(BufferSpec{.name = std::move(spec.name),
                                                   .initial_text = std::move(spec.initial_text),
                                                   .kind = spec.kind,
                                                   .resource_uri = std::move(spec.resource),
                                                   .read_only = spec.read_only},
                                        spec.style, std::move(spec.style_origin), spec.major_mode);
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
                   return std::vector<WindowId>(window_layout().leaves().begin(),
                                                window_layout().leaves().end());
               },
           .active_window = [this] { return window_id(); },
           .set_window_role =
               [this](WindowId window, std::optional<std::string> role) {
                   return set_window_role(window, std::move(role));
               },
           .set_window_pinned = [this](WindowId window,
                                       bool pinned) { return set_window_pinned(window, pinned); },
           .workbench_slot =
               [this](WorkbenchId workbench, std::string_view role) {
                   return workbench_slot(workbench, role);
               },
           .focus_window = [this](WindowId window) -> std::expected<void, std::string> {
               return focus_window(window) ? std::expected<void, std::string>{}
                                           : std::unexpected("unknown window");
           },
           .request_redraw = [this] { reveal_caret_ = true; },
           .set_caret_reveal = [this](bool reveal) { reveal_caret_ = reveal; },
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
           .soft_kill_range = [this](ViewId view, bool structural)
               -> std::expected<std::optional<GuileTextRange>, std::string> {
               EditSession& active = session_for(view);
               if (structural && !active.has_language_facet(LanguageFacet::StructuralEditing)) {
                   return std::unexpected(
                       "structural kill range is unavailable for the current mode");
               }
               const DocumentSnapshot snapshot = active.snapshot();
               const TextRange range =
                   structural
                       ? soft_kill_end(active.analysis(LanguageFacet::StructuralEditing).tree,
                                       snapshot.content(), active.caret())
                       : plain_kill_line_range(snapshot.content(), active.caret());
               if (range.empty()) {
                   return std::optional<GuileTextRange>{};
               }
               return std::optional<GuileTextRange>{
                   GuileTextRange{range.start.value, range.end.value}};
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
                   !active.has_language_facet(LanguageFacet::StructuralMotion)) {
                   return std::unexpected("structural motion is unavailable for the current mode");
               }
               StructuralMotionResolver resolver;
               if (active.has_language_facet(LanguageFacet::StructuralMotion)) {
                   resolver = [&active](StructuralMotion motion, TextOffset from) {
                       return active.move_structurally(from, motion);
                   };
               }
               return evaluate_motion(runtime_.motions(), *motion, active.snapshot(), source, count,
                                      extend, resolver);
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
            guile_.project_index_updated(project);
        });
    register_input_states();
    register_modes();
    register_resource_policies();
    register_buffer_lifecycle_policies();
    register_pointer_policies();
    register_commands();
    register_interaction_providers();
    register_keymaps();
    register_presentation_policies();
    if (spec.init_file) {
        if (std::expected<void, std::string> loaded = guile_.load_extension(*spec.init_file);
            !loaded) {
            message_ = std::format("init failed: {}", loaded.error());
        }
    }
    const std::expected<PresentationProfile, std::string> presentation_profile =
        guile_.presentation_profile();
    if (!presentation_profile) {
        throw std::runtime_error(
            std::format("Guile presentation policy failed: {}", presentation_profile.error()));
    }
    presentation_profile_ = *presentation_profile;

    std::expected<StartupPlan, std::string> startup = guile_.startup_plan(
        {.requested_resource = spec.path, .has_initial_text = spec.initial_text.has_value()});
    if (!startup) {
        throw std::runtime_error(std::format("Guile startup policy failed: {}", startup.error()));
    }
    std::string initial_text;
    if (startup->buffer.use_initial_text) {
        if (!spec.initial_text) {
            throw std::logic_error("validated startup plan requested unavailable initial text");
        }
        initial_text = std::move(*spec.initial_text);
    }
    const BufferId initial =
        create_buffer(BufferSpec{.name = std::move(startup->buffer.name),
                                 .initial_text = std::move(initial_text),
                                 .kind = startup->buffer.kind,
                                 .resource_uri = std::move(startup->buffer.resource),
                                 .read_only = startup->buffer.read_only},
                      startup->style, std::move(startup->style_origin), startup->buffer.major_mode);
    const ViewId initial_view = create_view({}, initial);
    const WindowId initial_window = runtime_.windows().create(initial_view);
    view_state_for(initial_view).window = initial_window;
    const WorkbenchId initial_workbench =
        workbenches_.create(WorkbenchSpec{.name = {}, .root_window = initial_window, .scope = {}});
    workbenches_.get(initial_workbench).visit_buffer(initial);
    if (spec.initial_line > 0 && !startup->resource_to_open) {
        apply_position(initial_window, {.line = spec.initial_line - 1, .byte_column = 0});
    }

    sync_keymaps();
    if (std::expected<void, std::string> recorded = guile_.set_startup_placeholder(
            startup->startup_placeholder ? std::optional(initial) : std::nullopt);
        !recorded) {
        throw std::runtime_error(
            std::format("Guile startup policy state failed: {}", recorded.error()));
    }
    if (startup->resource_to_open) {
        if (std::expected<void, std::string> opened = guile_.open_resource(
                initial_window, *startup->resource_to_open,
                spec.initial_line > 0 ? std::optional(spec.initial_line - 1) : std::nullopt,
                spec.initial_line > 0 ? std::optional<std::uint32_t>(0) : std::nullopt);
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
    return runtime_.windows().get(window_id()).view_id();
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
    return syntax_tokens(window_id());
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
    message_.clear();
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

ChromeContent EditorApplication::chrome_content(std::string_view preedit) {
    ChromeFacts facts;
    facts.message = message_;
    facts.preedit = preedit;
    facts.pending_sequence = pending_key_sequence_text();
    facts.pending_prefix = pending_prefix_text();
    if (const InteractionState* interaction = interaction_.state()) {
        facts.interaction = interaction->request.kind == InteractionKind::Picker
                                ? ChromeInteractionKind::Picker
                                : ChromeInteractionKind::Text;
        facts.prompt = interaction->request.prompt;
        facts.input = interaction_.input_text();
        facts.input_caret = interaction_.input_caret().value;
        if (!interaction->candidates.empty()) {
            facts.selection = interaction->selected;
        }
        facts.candidates.reserve(interaction->candidates.size());
        for (const InteractionCandidate& candidate : interaction->candidates) {
            facts.candidates.push_back({.label = candidate.label, .detail = candidate.detail});
        }
    }
    const std::vector<KeyBindingHint> hints = pending_key_hints();
    facts.hints.reserve(hints.size());
    for (const KeyBindingHint& hint : hints) {
        facts.hints.push_back({.key = hint.key, .detail = hint.detail, .prefix = hint.prefix});
    }
    std::expected<ChromeContent, std::string> content =
        guile_.chrome_content(command_context(), facts);
    if (!content) {
        ChromeContent fallback;
        fallback.echo = std::format("presentation policy failed: {}", content.error());
        return fallback;
    }
    if (content->echo_cursor_byte) {
        content->echo_cursor_column = ui::display_width(
            std::string_view(content->echo).substr(0, *content->echo_cursor_byte));
    }
    return std::move(*content);
}

ModelineContent EditorApplication::modeline(WindowId window_id) {
    EditSession& active = session(window_id);
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    const LinePosition position = text.position(active.caret());
    const Buffer& buffer = active.buffer();
    const View& view = active.view();
    CommandContext context(runtime_, window_id, buffer.id(), view.id());
    const InputStateRegistry::Definition& state = input_state(window_id);
    const ModelineFacts facts{
        .buffer_name = buffer.name(),
        .resource = buffer.resource_uri().value_or(std::string()),
        .dirty = buffer.modified(),
        .line = position.line + 1,
        .column = static_cast<std::uint32_t>(
            ui::display_column(text, active.caret(), active.style().tab_width) + 1),
        .line_count = text.line_count(),
        .revision = snapshot.revision(),
        .style_origin = style_origin(window_id),
        .last_key = window_id == this->window_id() ? last_key_ : std::string(),
        .input_state = state.indicator,
    };
    std::expected<ModelineContent, std::string> content = guile_.modeline_content(context, facts);
    return content ? std::move(*content) : ModelineContent{};
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
    return execute_command(*command, invocation);
}

bool EditorApplication::execute_command(CommandId command, const CommandInvocation& invocation) {
    const RevisionId interaction_revision = interaction_.input_revision();
    CommandContext context = command_context();
    const bool consumed = handle_loop_result(command_loop_.execute(command, context, invocation));
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
    return input_state(window_id());
}

const InputStateRegistry::Definition& EditorApplication::input_state(WindowId window) const {
    const std::optional<InputStateId> state =
        runtime_.views().get(view_id(window)).input_states().top();
    if (!state) {
        throw std::logic_error("view has no input state");
    }
    return runtime_.input_states().definition(*state);
}

bool EditorApplication::handle_pointer(const PointerEvent& event) {
    const std::expected<bool, std::string> handled =
        guile_.handle_pointer(command_context(), event, !command_loop_.pending_sequence().empty());
    return handled.value_or(false);
}

bool EditorApplication::handle_scroll(ScrollInput input) {
    const std::expected<bool, std::string> handled = guile_.handle_scroll(command_context(), input);
    return handled.value_or(false);
}

bool EditorApplication::request_close(bool force) {
    const std::expected<CommandId, std::string> command =
        guile_.close_command(command_context(), force);
    if (!command) {
        message_ = std::format("close policy failed: {}", command.error());
        return false;
    }
    return execute_command(*command, {});
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
    return guile_.open_resource(window_id(), input);
}

bool EditorApplication::switch_buffer(BufferId buffer) {
    return show_buffer(window_id(), buffer);
}

bool EditorApplication::focus_window(WindowId window) {
    Workbench& workbench = active_workbench();
    if (!workbench.layout().contains(window) || runtime_.windows().try_get(window) == nullptr) {
        return false;
    }
    if (window != workbench.active_window()) {
        command_loop_.cancel_pending();
    }
    workbench.set_active_window(window);
    workbench.visit_buffer(buffer_id(window));
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

bool EditorApplication::split_window(WindowSplitAxis axis) {
    return split_window(window_id(), axis);
}

bool EditorApplication::split_window(WindowId target, WindowSplitAxis axis) {
    Workbench& workbench = active_workbench();
    if (!workbench.layout().contains(target) || runtime_.windows().try_get(target) == nullptr) {
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
    if (!workbench.layout().split({.target = target, .new_window = window, .axis = axis})) {
        destroy_window(window);
        return false;
    }
    reveal_caret_ = true;
    return true;
}

bool EditorApplication::delete_window() {
    return delete_window(window_id());
}

bool EditorApplication::delete_window(WindowId target) {
    Workbench& workbench = active_workbench();
    const std::optional<WindowId> replacement = workbench.layout().next(target);
    if (!replacement || *replacement == target || !workbench.layout().erase(target)) {
        return false;
    }
    if (workbench.active_window() == target) {
        workbench.set_active_window(*replacement);
    }
    destroy_window(target);
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

bool EditorApplication::delete_other_windows() {
    return delete_other_windows(window_id());
}

std::expected<void, std::string>
EditorApplication::set_window_role(WindowId window, std::optional<std::string> role) {
    Window* target = runtime_.windows().try_get(window);
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    if (target == nullptr || !owner) {
        return std::unexpected("unknown workbench window");
    }
    if (role && role->empty()) {
        return std::unexpected("window role must not be empty");
    }
    Workbench& workbench = workbenches_.get(*owner);
    if (target->role()) {
        workbench.clear_slot(*target->role());
    }
    if (role) {
        if (const std::optional<WindowId> previous = workbench.slot(*role);
            previous && *previous != window) {
            runtime_.windows().get(*previous).set_role(std::nullopt);
        }
        workbench.set_slot(*role, window);
    }
    target->set_role(std::move(role));
    return {};
}

std::expected<void, std::string> EditorApplication::set_window_pinned(WindowId window,
                                                                      bool pinned) {
    Window* target = runtime_.windows().try_get(window);
    if (target == nullptr || !workbenches_.find_by_window(window)) {
        return std::unexpected("unknown workbench window");
    }
    target->set_pinned(pinned);
    return {};
}

std::expected<void, std::string> EditorApplication::set_window_created_by_policy(WindowId window,
                                                                                 bool created) {
    Window* target = runtime_.windows().try_get(window);
    if (target == nullptr || !workbenches_.find_by_window(window)) {
        return std::unexpected("unknown workbench window");
    }
    target->set_created_by_policy(created);
    return {};
}

std::optional<WindowId> EditorApplication::workbench_slot(WorkbenchId workbench,
                                                          std::string_view role) const {
    const Workbench* target = workbenches_.try_get(workbench);
    return target == nullptr ? std::nullopt : target->slot(role);
}

bool EditorApplication::delete_other_windows(WindowId retained) {
    Workbench& workbench = active_workbench();
    if (!workbench.layout().contains(retained) || runtime_.windows().try_get(retained) == nullptr) {
        return false;
    }
    if (workbench.layout().leaves().size() <= 1) {
        return true;
    }
    const std::vector<WindowId> windows(workbench.layout().leaves().begin(),
                                        workbench.layout().leaves().end());
    (void)workbench.layout().retain(retained);
    for (const WindowId window : windows) {
        if (window != retained) {
            destroy_window(window);
        }
    }
    workbench.set_active_window(retained);
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

WorkbenchId EditorApplication::create_workbench(std::string name,
                                                std::optional<ProjectId> project) {
    if (name.empty()) {
        throw std::invalid_argument("workbench name must not be empty");
    }
    if (workbenches_.find_by_name(name)) {
        throw std::invalid_argument(std::format("workbench '{}' already exists", name));
    }
    if (project) {
        (void)runtime_.projects().get(*project);
    }

    EditSession& source = session();
    const ViewportState source_viewport = source.view().viewport();
    const std::optional<ViewSelection> source_selection = source.active_selection();
    const BufferId buffer = buffer_id();
    const ViewId view = create_view({}, buffer, source.caret());
    runtime_.views().get(view).viewport() = source_viewport;
    if (source_selection) {
        runtime_.views().set_selection(view, *source_selection);
    }
    const WindowId window = runtime_.windows().create(view);
    view_state_for(view).window = window;

    WorkbenchId workbench;
    try {
        std::vector<ProjectId> scope;
        if (project) {
            scope.push_back(*project);
        }
        workbench = workbenches_.create(WorkbenchSpec{
            .name = std::move(name), .root_window = window, .scope = std::move(scope)});
        workbenches_.get(workbench).visit_buffer(buffer);
    } catch (...) {
        destroy_window(window);
        throw;
    }
    if (!switch_workbench(workbench)) {
        (void)workbenches_.erase(workbench);
        destroy_window(window);
        throw std::logic_error("created workbench cannot be activated");
    }
    return workbench;
}

bool EditorApplication::switch_workbench(WorkbenchId workbench) {
    if (workbenches_.try_get(workbench) == nullptr) {
        return false;
    }
    if (workbench != workbenches_.active_id()) {
        command_loop_.cancel_pending();
    }
    if (!workbenches_.activate(workbench)) {
        return false;
    }
    Workbench& selected = workbenches_.active();
    selected.visit_buffer(buffer_id(selected.active_window()));
    reveal_caret_ = true;
    sync_keymaps();
    return true;
}

bool EditorApplication::close_workbench(WorkbenchId workbench) {
    Workbench* closing = workbenches_.try_get(workbench);
    if (closing == nullptr || workbenches_.size() <= 1) {
        return false;
    }
    const std::vector<WindowId> windows(closing->layout().leaves().begin(),
                                        closing->layout().leaves().end());
    const bool was_active = workbench == workbenches_.active_id();
    if (was_active) {
        const std::optional<WorkbenchId> replacement = workbenches_.next(workbench);
        if (!replacement || *replacement == workbench || !workbenches_.activate(*replacement)) {
            return false;
        }
    }
    if (!workbenches_.erase(workbench)) {
        return false;
    }
    for (const WindowId window : windows) {
        destroy_window(window);
    }
    if (was_active) {
        Workbench& selected = workbenches_.active();
        selected.visit_buffer(buffer_id(selected.active_window()));
        command_loop_.cancel_pending();
        reveal_caret_ = true;
        sync_keymaps();
    }
    return true;
}

bool EditorApplication::adopt_project(WorkbenchId workbench, ProjectId project) {
    (void)runtime_.projects().get(project);
    Workbench* target = workbenches_.try_get(workbench);
    return target != nullptr && target->adopt_project(project);
}

bool EditorApplication::expel_buffer(WorkbenchId workbench, BufferId buffer) {
    (void)runtime_.buffers().get(buffer);
    Workbench* target = workbenches_.try_get(workbench);
    return target != nullptr && target->expel_buffer(buffer);
}

std::vector<BufferId> EditorApplication::workbench_buffers(WorkbenchId workbench,
                                                           bool widen) const {
    const Workbench& target = workbenches_.get(workbench);
    std::vector<BufferId> result;
    result.reserve(buffers_.size());
    const auto append = [&](BufferId buffer) {
        if (runtime_.buffers().try_get(buffer) != nullptr &&
            std::ranges::find(result, buffer) == result.end()) {
            result.push_back(buffer);
        }
    };
    if (widen) {
        for (const std::unique_ptr<BufferState>& state : buffers_) {
            append(state->buffer);
        }
        return result;
    }
    for (const BufferId buffer : target.mru()) {
        append(buffer);
    }
    for (const std::unique_ptr<BufferState>& state : buffers_) {
        const Buffer& buffer = runtime_.buffers().get(state->buffer);
        if (buffer.project_id() && target.contains_project(*buffer.project_id())) {
            append(state->buffer);
        }
    }
    return result;
}

std::vector<WorkbenchSnapshot> EditorApplication::workbench_snapshots() const {
    std::vector<WorkbenchSnapshot> result;
    result.reserve(workbenches_.size());
    for (const WorkbenchId id : workbenches_.all()) {
        const Workbench& workbench = workbenches_.get(id);
        result.push_back(
            {.workbench = id,
             .name = workbench.name(),
             .scope = std::vector<ProjectId>(workbench.scope().begin(), workbench.scope().end()),
             .mru = std::vector<BufferId>(workbench.mru().begin(), workbench.mru().end()),
             .windows = std::vector<WindowId>(workbench.layout().leaves().begin(),
                                              workbench.layout().leaves().end()),
             .active_window = workbench.active_window(),
             .active = id == workbenches_.active_id()});
    }
    return result;
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
    workbenches_.forget_buffer(buffer);
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
        const ViewState* view = find_view(window_id(), state.buffer);
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
    result.reserve(window_layout().leaves().size());
    for (const WindowId window : window_layout().leaves()) {
        const Window& editor_window = runtime_.windows().get(window);
        const ViewId view = editor_window.view_id();
        result.push_back({.window = window,
                          .view = view,
                          .buffer = runtime_.views().get(view).buffer_id(),
                          .role = editor_window.role(),
                          .pinned = editor_window.pinned(),
                          .created_by_policy = editor_window.created_by_policy(),
                          .active = window == window_id()});
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
    if (const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window)) {
        workbenches_.get(*owner).visit_buffer(buffer);
    }
    reveal_caret_ = true;
    if (window == window_id()) {
        sync_keymaps();
    }
    return true;
}

std::expected<void, std::string>
EditorApplication::display_generated_buffer(WindowId window, std::string name, std::string text,
                                            ModeId mode, std::string style_origin) {
    try {
        (void)runtime_.modes().definition(mode);
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
            target.modes().set_major(runtime_.modes(), mode);
            state_for(buffer).style_origin = std::move(style_origin);
        } else {
            buffer = create_buffer(BufferSpec{.name = std::move(name),
                                              .initial_text = std::move(text),
                                              .kind = BufferKind::Generated,
                                              .resource_uri = std::nullopt,
                                              .read_only = true},
                                   CppIndentStyle{}, std::move(style_origin), mode);
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

void EditorApplication::scroll_view_lines(ViewId view, double lines) {
    if (!std::isfinite(lines)) {
        throw std::invalid_argument("scroll delta must be finite");
    }
    EditSession& target = session_for(view);
    const DocumentSnapshot snapshot = target.snapshot();
    const double last_line = static_cast<double>(snapshot.content().line_count() - 1);
    ViewportState& viewport = target.view().viewport();
    const double position = static_cast<double>(viewport.top_line) +
                            static_cast<double>(viewport.top_line_offset) + lines;
    const double clamped = std::clamp(position, 0.0, last_line);
    const double integral = std::floor(clamped);
    viewport.top_line = static_cast<std::uint32_t>(integral);
    viewport.top_line_offset = static_cast<float>(clamped - integral);
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
    for (const WorkbenchId workbench : workbenches_.all()) {
        workbenches_.get(workbench).clear_window_slots(window);
    }
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

void EditorApplication::register_commands() {
    std::expected<std::size_t, std::string> installed = guile_.install_core_commands();
    if (!installed) {
        throw std::runtime_error(std::format("Guile command policy failed: {}", installed.error()));
    }
}

void EditorApplication::register_buffer_lifecycle_policies() {
    if (std::expected<void, std::string> installed = guile_.install_buffer_lifecycle_policies();
        !installed) {
        throw std::runtime_error(
            std::format("Guile buffer lifecycle policy failed: {}", installed.error()));
    }
}

void EditorApplication::register_pointer_policies() {
    if (std::expected<void, std::string> installed = guile_.install_pointer_policies();
        !installed) {
        throw std::runtime_error(std::format("Guile pointer policy failed: {}", installed.error()));
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
}

void EditorApplication::register_presentation_policies() {
    if (std::expected<void, std::string> installed = guile_.install_presentation_policies();
        !installed) {
        throw std::runtime_error(
            std::format("Guile presentation policy failed: {}", installed.error()));
    }
}

std::vector<KeymapLayer> EditorApplication::base_keymap_layers(WindowId window_id) {
    const Window& window = runtime_.windows().get(window_id);
    const View& view = runtime_.views().get(window.view_id());
    CommandContext context(runtime_, window_id, view.buffer_id(), view.id());
    const std::expected<GuileKeymapPolicy, std::string> policy = guile_.base_keymap_policy(context);
    if (!policy) {
        throw std::runtime_error(std::format("Guile keymap policy failed: {}", policy.error()));
    }
    std::vector<KeymapLayer> layers;
    layers.reserve(policy->layers.size());
    for (const GuileKeymapLayer& layer : policy->layers) {
        layers.push_back({.keymap = layer.keymap, .scope = layer.scope});
    }
    return layers;
}

void EditorApplication::sync_keymaps() {
    CommandContext context = command_context();
    const std::expected<GuileKeymapPolicy, std::string> policy = guile_.keymap_policy(context);
    if (!policy) {
        message_ = std::format("keymap policy failed: {}", policy.error());
        return;
    }
    std::vector<KeymapLayer> layers;
    layers.reserve(policy->layers.size());
    for (const GuileKeymapLayer& layer : policy->layers) {
        layers.push_back({.keymap = layer.keymap, .scope = layer.scope});
    }
    const std::span<const KeymapLayer> active = command_loop_.keymap_layers();
    const bool layers_changed =
        layers.size() != active.size() ||
        !std::ranges::equal(layers, active, [](const KeymapLayer& left, const KeymapLayer& right) {
            return left.keymap == right.keymap && left.scope == right.scope;
        });
    const std::span<const KeymapId> active_overrides = command_loop_.override_keymaps();
    const bool overrides_changed = policy->overrides.size() != active_overrides.size() ||
                                   !std::ranges::equal(policy->overrides, active_overrides);
    if (layers_changed) {
        command_loop_.set_keymap_layers(std::move(layers));
    }
    if (overrides_changed) {
        command_loop_.set_override_keymaps(policy->overrides);
    }
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
    return CommandContext(runtime_, window_id(), buffer_id(), view_id());
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

std::expected<std::string, std::string> EditorApplication::begin_buffer_save(BufferId buffer) {
    BufferState& state = state_for(buffer);
    if (state.pending_save) {
        return std::unexpected("save already in progress");
    }
    try {
        Text content = runtime_.buffers().get(state.buffer).snapshot().content();
        std::string serialized = content.to_string();
        state.pending_save.emplace(PendingSave{.content = std::move(content)});
        return serialized;
    } catch (const std::exception& exception) {
        state.pending_save.reset();
        return std::unexpected(exception.what());
    }
}

std::expected<bool, std::string> EditorApplication::complete_buffer_save(BufferId buffer) {
    BufferState& state = state_for(buffer);
    if (!state.pending_save) {
        return std::unexpected("buffer has no save in progress");
    }
    mark_saved(buffer, std::move(state.pending_save->content));
    state.pending_save.reset();
    return runtime_.buffers().get(buffer).modified();
}

void EditorApplication::abort_buffer_save(BufferId buffer) {
    state_for(buffer).pending_save.reset();
}

void EditorApplication::mark_saved(BufferId buffer, Text content) {
    BufferState& state = state_for(buffer);
    runtime_.buffers().get(buffer).mark_saved(std::move(content));
    ++state.save_generation;
}

} // namespace cind
