#include "editor/editor_application.hpp"

#include "editor/noun_evaluator.hpp"
#include "editor/resource_policy.hpp"
#include "lsp/completion.hpp"
#include "lsp/diagnostics.hpp"
#include "lsp/navigation.hpp"
#include "syntax/structure.hpp"
#include "ui/char_width.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

constexpr std::size_t default_jump_graph_limit = 4096;

TextRange plain_kill_line_range(const Text& text, TextOffset caret) {
    const std::uint32_t line = text.position(caret).line;
    const TextOffset content_end = text.line_content_end(line);
    if (caret < content_end) {
        return {caret, content_end};
    }
    const TextOffset line_end = text.line_range(line).end;
    return caret < line_end ? TextRange{caret, line_end} : TextRange{caret, caret};
}

CompletionProviderResponse completion_response_from_lsp(CompletionProvider provider,
                                                        const CompletionRequest& request,
                                                        const Text& text,
                                                        LspCompletionResponse response) {
    std::vector<CompletionItem> items;
    items.reserve(response.items.size());
    for (LspCompletionItem& source : response.items) {
        CompletionEdit edit{.insert_range = {request.anchor, request.caret},
                            .replace_range = {request.anchor, request.caret},
                            .new_text =
                                source.insert_text.empty() ? source.label : source.insert_text};
        if (source.edit) {
            const std::optional<TextRange> insert =
                text_range_from_lsp(text, source.edit->insert_range);
            const std::optional<TextRange> replace =
                text_range_from_lsp(text, source.edit->replace_range);
            if (!insert || !replace) {
                throw std::runtime_error("LSP completion returned an invalid text edit range");
            }
            edit = {.insert_range = *insert,
                    .replace_range = *replace,
                    .new_text = std::move(source.edit->new_text)};
        }
        std::vector<TextEdit> additional;
        additional.reserve(source.additional_edits.size());
        for (LspTextEdit& source_edit : source.additional_edits) {
            const std::optional<TextRange> range = text_range_from_lsp(text, source_edit.range);
            if (!range) {
                throw std::runtime_error(
                    "LSP completion returned an invalid additional edit range");
            }
            additional.push_back(
                {.old_range = *range, .new_text = std::move(source_edit.new_text)});
        }
        items.push_back({.provider = provider,
                         .filter_text = std::move(source.filter_text),
                         .label = std::move(source.label),
                         .kind = std::move(source.kind),
                         .detail = std::move(source.detail),
                         .edit = std::move(edit),
                         .sort_text = std::move(source.sort_text),
                         .is_snippet = source.is_snippet,
                         .resolved = source.resolved,
                         .resolving = false,
                         .resolve_error = {},
                         .documentation = std::move(source.documentation),
                         .additional_edits = std::move(additional),
                         .raw = std::move(source.raw)});
    }
    return {
        .provider = provider, .items = std::move(items), .is_incomplete = response.is_incomplete};
}

struct LspCompletionBridge {
    CompletionProvider provider;
    CompletionRequest request;
    LspCompletionRequest lsp_request;
};

void report_completion_failure(const CompletionProviderAsync::Failed& failed,
                               std::string_view message) noexcept {
    try {
        failed(std::string(message));
    } catch (...) {
        return;
    }
}

} // namespace

EditorApplication::EditorApplication(EditorApplicationSpec spec)
    : guile_(
          runtime_,
          {.display_buffer =
               [this](WindowId origin, BufferId buffer, std::string_view intent,
                      std::optional<GuileDisplayPosition> position) {
                   std::optional<LinePosition> target;
                   if (position) {
                       const Buffer& target_buffer = runtime_.buffers().get(buffer);
                       const DocumentSnapshot snapshot = target_buffer.snapshot();
                       const Text& text = snapshot.content();
                       EncodedLinePosition requested = position->position;
                       requested.line = std::min(requested.line, text.line_count() - 1);
                       target = resolve_line_position(text, requested);
                       if (!target) {
                           target = LinePosition{
                               .line = requested.line,
                               .byte_column = text.line_content_range(requested.line).length()};
                       }
                   }
                   return display_buffer(buffer, intent, origin, target);
               },
           .display_generated_buffer =
               [this](WindowId window, std::string name, std::string text, ModeId mode,
                      std::string style_origin, std::string_view intent) {
                   return display_generated_buffer(window, std::move(name), std::move(text), mode,
                                                   std::move(style_origin), intent);
               },
           .navigate_jump = [this](WindowId window,
                                   std::int64_t delta) { return navigate_jump(window, delta); },
           .mark_jump = [this](WindowId window) { return mark_jump(window); },
           .visit_jump = [this](WindowId window,
                                std::uint64_t node) { return visit_jump(window, node); },
           .link_jump =
               [this](WindowId window, std::uint64_t from, std::uint64_t to, std::string_view kind,
                      bool persistent) {
                   return link_jump(window, from, to, std::string(kind), persistent);
               },
           .jump_branches =
               [this](WindowId window, bool incoming) {
                   std::vector<GuileJumpEdge> result;
                   for (const JumpEdge& edge : jump_branches(window, incoming)) {
                       result.push_back({.from = edge.from,
                                         .to = edge.to,
                                         .kind = edge.kind,
                                         .at = edge.at,
                                         .persistent = edge.persistent});
                   }
                   return result;
               },
           .jump_node = [this](WindowId window,
                               std::uint64_t node) -> std::optional<GuileJumpNode> {
               const std::optional<JumpNode> found = jump_node(window, node);
               if (!found) {
                   return std::nullopt;
               }
               return GuileJumpNode{.id = found->id,
                                    .resource = found->position.resource,
                                    .line = found->position.fallback.line,
                                    .byte_column = found->position.fallback.byte_column,
                                    .excerpt = found->position.excerpt,
                                    .last_visit = found->last_visit};
           },
           .evict_jumps = [this](WindowId window,
                                 std::size_t maximum) { return evict_jumps(window, maximum); },
           .move_caret_to_line =
               [this](ViewId view, std::uint32_t line, std::uint32_t display_column) {
                   return move_caret_to_line(view, line, display_column);
               },
           .undo = [this](ViewId view) { return editing_mechanisms_.undo(view); },
           .redo = [this](ViewId view) { return editing_mechanisms_.redo(view); },
           .set_view_caret =
               [this](ViewId view, std::uint32_t offset) {
                   session_for(view).set_caret(TextOffset{offset});
                   show_caret();
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
           .structural_edit =
               [this](ViewId view, std::string_view operation) {
                   const auto edit =
                       operation == "splice"          ? std::optional{StructuralEdit::Splice}
                       : operation == "forward-slurp" ? std::optional{StructuralEdit::ForwardSlurp}
                       : operation == "forward-barf"  ? std::optional{StructuralEdit::ForwardBarf}
                       : operation == "backward-slurp"
                           ? std::optional{StructuralEdit::BackwardSlurp}
                       : operation == "backward-barf" ? std::optional{StructuralEdit::BackwardBarf}
                                                      : std::nullopt;
                   if (!edit) {
                       return std::expected<void, std::string>{
                           std::unexpected("unknown structural edit operation")};
                   }
                   return editing_mechanisms_.edit_structure(view, *edit);
               },
           .interaction_mechanism_status =
               [this] {
                   const InteractionMechanismState* state = interaction_.state();
                   return GuileInteractionMechanismStatus{
                       .active = state != nullptr,
                       .candidate_count =
                           state != nullptr ? state->candidates.size() : std::size_t{0},
                       .buffer = state != nullptr ? std::optional(state->buffer) : std::nullopt,
                       .view = state != nullptr ? std::optional(state->view) : std::nullopt,
                       .candidate_revision =
                           state != nullptr ? state->candidate_revision : std::uint64_t{0}};
               },
           .interaction_origin_project = [this]() -> std::optional<ProjectId> {
               const InteractionMechanismState* state = interaction_.state();
               if (state == nullptr) {
                   return std::nullopt;
               }
               const Buffer* buffer = runtime_.buffers().try_get(state->origin.buffer);
               return buffer != nullptr ? buffer->project_id() : std::nullopt;
           },
           .refresh_interaction =
               [this](std::string_view provider) {
                   return interaction_.refresh_candidates(provider);
               },
           .submit_interaction =
               [this](std::optional<std::size_t> selected, bool allow_custom_input) {
                   std::expected<std::string, std::string> submission =
                       interaction_.submit(selected, allow_custom_input);
                   if (submission) {
                       interaction_session_.reset();
                   }
                   return submission;
               },
           .replace_interaction_input =
               [this](std::string_view input) { return interaction_.replace_input(input); },
           .cancel_interaction =
               [this] {
                   interaction_session_.reset();
                   return interaction_.cancel();
               },
           .completion_active = [this] { return completion_ && completion_->active(); },
           .refresh_completion = [this] { return refresh_completion(); },
           .ensure_lsp_session =
               [this](CommandTarget target, ScriptLspProviderSpec provider) {
                   return ensure_lsp_session(target, std::move(provider));
               },
           .attach_lsp_diagnostics =
               [this](std::uint64_t session) {
                   return attach_lsp_diagnostics(LspSessionId{session});
               },
           .synchronize_lsp_session =
               [this](BufferId buffer, std::uint64_t session) {
                   return synchronize_lsp_session(buffer, LspSessionId{session});
               },
           .start_completion =
               [this](CommandTarget target, TextOffset anchor,
                      std::vector<CompletionProvider> providers, CompletionTrigger trigger) {
                   return start_completion(target, anchor, std::move(providers),
                                           std::move(trigger));
               },
           .move_completion = [this](std::int64_t delta) { return move_completion(delta); },
           .apply_completion = [this](bool replace) { return apply_completion(replace); },
           .cancel_completion = [this] { return cancel_completion(); },
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
           .view_line_prefix =
               [this](ViewId view) {
                   const EditSession& active = session_for(view);
                   const DocumentSnapshot snapshot = active.snapshot();
                   const Text& text = snapshot.content();
                   const TextOffset caret = active.caret();
                   const TextOffset line_start = text.line_start(text.position(caret).line);
                   return GuileViewLinePrefix{.line_start = line_start.value,
                                              .caret = caret.value,
                                              .text = text.substring({line_start, caret})};
               },
           .view_syntax_token = [this](ViewId view,
                                       TextOffset offset) -> std::optional<GuileSyntaxToken> {
               const EditSession& active = session_for(view);
               const Buffer& buffer = active.buffer();
               const DocumentSnapshot snapshot = active.snapshot();
               if (offset.value > snapshot.content().size_bytes() ||
                   !runtime_.language_provider(buffer.id(), LanguageFacet::Highlighting)) {
                   return std::nullopt;
               }
               const TokenBuffer& tokens =
                   active.analysis(LanguageFacet::Highlighting).tree.tokens();
               const auto token = std::ranges::find_if(
                   tokens, [offset](Token candidate) { return candidate.range.contains(offset); });
               if (token == tokens.end()) {
                   return std::nullopt;
               }
               return GuileSyntaxToken{.kind = std::string(token_kind_name(token->kind)),
                                       .start = token->range.start.value,
                                       .end = token->range.end.value};
           },
           .view_identifier_words =
               [this](ViewId view) {
                   const EditSession& active = session_for(view);
                   const Buffer& buffer = active.buffer();
                   if (!runtime_.language_provider(buffer.id(), LanguageFacet::Highlighting)) {
                       return std::vector<std::string>{};
                   }
                   const DocumentSnapshot snapshot = active.snapshot();
                   std::set<std::string, std::less<>> words;
                   for (const Token token :
                        active.analysis(LanguageFacet::Highlighting).tree.tokens()) {
                       if (token.kind == TokenKind::Identifier) {
                           words.insert(snapshot.content().substring(token.range));
                       }
                   }
                   return std::vector<std::string>(words.begin(), words.end());
               },
           .publish_location_list =
               [this](WindowId window, BufferId buffer, std::string source,
                      std::vector<BufferLocation> locations) {
                   return publish_location_list(window, buffer, std::move(source),
                                                std::move(locations));
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
                   if (active_workbench().location_lists().current() == nullptr && !selected) {
                       return {};
                   }
                   return active_workbench().location_lists().select(selected)
                              ? std::expected<void, std::string>{}
                              : std::unexpected("location index is out of range");
               }
               Workbench* owner = nullptr;
               for (const WorkbenchId workbench : workbenches_.all()) {
                   Workbench& candidate = workbenches_.get(workbench);
                   if (candidate.location_lists().find_by_buffer(*buffer) != nullptr) {
                       owner = &candidate;
                       break;
                   }
               }
               if (owner == nullptr || !owner->location_lists().set_current_by_buffer(*buffer)) {
                   return std::unexpected("location list is no longer available");
               }
               if (!owner->location_lists().select(selected)) {
                   return std::unexpected("location index is out of range");
               }
               return {};
           },
           .location_target = [this](std::optional<BufferId> buffer,
                                     std::size_t index) -> std::optional<GuileLocationTarget> {
               const std::optional<LocationItem> item = location_item(buffer, index);
               if (!item) {
                   return std::nullopt;
               }
               EncodedLinePosition position = item->range.start;
               bool stale = false;
               if (item->resolved) {
                   const Buffer* target = runtime_.buffers().try_get(item->resolved->buffer);
                   if (target != nullptr) {
                       position =
                           EncodedLinePosition::from_bytes(target->snapshot().content().position(
                               target->navigation_anchor_offset(item->resolved->start)));
                       stale = item->resolved->stale;
                   }
               }
               return GuileLocationTarget{
                   .resource = item->resource, .position = position, .stale = stale};
           },
           .move_location_list =
               [this](int delta) { return active_workbench().location_lists().move(delta); },
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
           .request_project_index = [this](ProjectId project) -> std::expected<void, std::string> {
               try {
                   (void)runtime_.projects().get(project);
                   project_service_->request_index(project);
                   return {};
               } catch (const std::exception& exception) {
                   return std::unexpected(exception.what());
               }
           },
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
           .workbench_session_state = [this] { return serialize_workbench_session(); },
           .prepare_workbench_session_restore =
               [this](std::string_view serialized) {
                   return prepare_workbench_session_restore(serialized);
               },
           .show_buffer_in_window = [this](WindowId window, BufferId buffer, std::uint32_t caret)
               -> std::expected<void, std::string> {
               if (!show_buffer(window, buffer)) {
                   return std::unexpected("cannot display restored buffer");
               }
               const TextOffset end =
                   runtime_.buffers().get(buffer).snapshot().content().end_offset();
               runtime_.views().set_caret(runtime_.windows().get(window).view_id(),
                                          TextOffset{std::min(caret, end.value)});
               return {};
           },
           .replace_workbench_mru =
               [this](WorkbenchId workbench,
                      const std::vector<BufferId>& buffers) -> std::expected<void, std::string> {
               Workbench* target = workbenches_.try_get(workbench);
               if (target == nullptr) {
                   return std::unexpected("unknown workbench");
               }
               for (const BufferId buffer : buffers) {
                   if (runtime_.buffers().try_get(buffer) == nullptr) {
                       return std::unexpected("workbench MRU contains an unknown buffer");
                   }
               }
               target->replace_mru(buffers);
               return {};
           },
           .window_buffer = [this](WindowId window) { return buffer_id(window); },
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
           .release_buffer =
               [this](BufferId buffer, BufferId replacement) {
                   return release_buffer(buffer, replacement);
               },
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
                   show_caret();
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
                       after_edit(view);
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
                   after_edit(view);
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
                       after_edit(view);
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
      editing_mechanisms_([this](ViewId view) -> EditSession& { return session_for(view); },
                          {.edited = [this](ViewId view) { after_edit(view); },
                           .caret_moved = [this](ViewId) { show_caret(); }}),
      command_loop_(runtime_), platform_services_(std::move(spec.platform_services)),
      async_runtime_(std::move(platform_services_.wake_event_loop)),
      script_async_(async_runtime_, {.start = [this](ScriptAsyncRequest request,
                                                     ScriptExternalAsyncCallbacks callbacks) {
                        const auto* navigation = std::get_if<ScriptLspNavigationRequest>(&request);
                        if (navigation == nullptr) {
                            return std::expected<ScriptAsyncExternalServices::Cancel, std::string>{
                                std::unexpected("unsupported external async request")};
                        }
                        return start_lsp_navigation(*navigation, std::move(callbacks));
                    }}) {
    interaction_.attach_async_runtime(async_runtime_);
    lsp_sessions_ = std::make_unique<LspSessionRegistry>(async_runtime_);
    completion_ = std::make_unique<CompletionPipeline>(
        runtime_, async_runtime_,
        [this](CompletionProvider provider, const CompletionRequest& request) {
            return dispatch_completion_provider(provider, request);
        },
        [this](const CompletionRequest& request, const CompletionItem& item,
               CompletionPipeline::ResolveCompleted completed,
               CompletionProviderAsync::Failed failed,
               CompletionProviderAsync::Cancelled cancelled) {
            return dispatch_completion_resolve(request, item, std::move(completed),
                                               std::move(failed), std::move(cancelled));
        },
        [this](CommandTarget target) {
            after_edit(target.view);
            sync_keymaps();
        });
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
    if (std::expected<void, std::string> installed = guile_.install_display_policy(); !installed) {
        throw std::runtime_error(std::format("Guile display policy failed: {}", installed.error()));
    }
    if (spec.init_file) {
        if (std::expected<void, std::string> loaded = guile_.load_extension(*spec.init_file);
            !loaded) {
            set_message(std::format("init failed: {}", loaded.error()));
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
            set_message(std::format("open failed: {}", opened.error()));
        }
    }
}

EditorApplication::~EditorApplication() {
    (void)completion_->cancel();
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
    record_command_input(format_key_stroke(key), true);
    if (const std::expected<void, std::string> updated =
            guile_.set_page_rows(static_cast<std::uint32_t>(std::max(1, page_rows)));
        !updated) {
        set_message(std::format("page geometry state failed: {}", updated.error()));
    }
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
                    set_message(handled.error());
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
                        set_message("input state handler returned an invalid command");
                        sync_keymaps();
                        return true;
                    }
                    if (handled->invocation.prefix.empty()) {
                        handled->invocation.prefix = command_loop_.pending_prefix();
                    }
                    const bool consumed = handle_loop_result(
                        command_loop_.execute(handled->command, context, handled->invocation));
                    sync_keymaps();
                    return consumed;
                }
                if (handled->kind == InputStateHandlerActionKind::Pending) {
                    if (!handled->feedback) {
                        command_loop_.cancel_pending();
                        set_message("input state handler returned pending without feedback");
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
    if (preserve_prefix_for_text) {
        command_loop_.set_pending_prefix(text_prefix);
    }
    sync_keymaps();
    return consumed;
}

GuileCommandFeedbackState EditorApplication::command_feedback() const {
    const std::expected<GuileCommandFeedbackState, std::string> state =
        guile_.command_feedback_state();
    return state.value_or(GuileCommandFeedbackState{});
}

bool EditorApplication::reveal_caret() const {
    return guile_.application_state().value_or(GuileApplicationState{}).reveal_caret;
}

void EditorApplication::show_caret() {
    if (const std::expected<void, std::string> updated = guile_.set_caret_reveal(true); !updated) {
        set_message(std::format("caret presentation state failed: {}", updated.error()));
    }
}

void EditorApplication::hide_caret() {
    if (const std::expected<void, std::string> updated = guile_.set_caret_reveal(false); !updated) {
        set_message(std::format("caret presentation state failed: {}", updated.error()));
    }
}

bool EditorApplication::should_quit() const {
    return guile_.application_state().value_or(GuileApplicationState{}).exit_requested;
}

void EditorApplication::set_message(std::string_view message) {
    if (const std::expected<void, std::string> updated = guile_.set_message(message); !updated) {
        return;
    }
}

ChromeContent EditorApplication::chrome_content(std::string_view preedit) {
    ChromeFacts facts;
    facts.preedit = preedit;
    facts.pending_sequence = pending_key_sequence_text();
    facts.pending_prefix = pending_prefix_text();
    if (const InteractionMechanismState* interaction = interaction_.state()) {
        const std::optional<GuileInteractionPolicyState> policy =
            guile_.interaction_policy_state().value_or(std::nullopt);
        facts.interaction = policy && policy->kind == InteractionKind::Picker
                                ? ChromeInteractionKind::Picker
                                : ChromeInteractionKind::Text;
        facts.prompt = policy ? policy->prompt : std::string{};
        facts.input = interaction_.input_text();
        facts.input_caret = interaction_.input_caret().value;
        facts.selection = guile_.interaction_selection().value_or(std::nullopt);
        facts.candidates.reserve(interaction->candidates.size());
        for (const InteractionCandidate& candidate : interaction->candidates) {
            facts.candidates.push_back(
                {.label = candidate.label, .detail = candidate.detail, .kind = {}});
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
        set_message(std::format("unknown command '{}'", name));
        sync_keymaps();
        return false;
    }
    return execute_command(*command, invocation);
}

bool EditorApplication::execute_command(CommandId command, const CommandInvocation& invocation) {
    CommandContext context = command_context();
    const bool consumed = handle_loop_result(command_loop_.execute(command, context, invocation));
    sync_keymaps();
    return consumed;
}

std::expected<void, std::string>
EditorApplication::start_completion(CommandTarget target, TextOffset anchor,
                                    std::vector<CompletionProvider> providers,
                                    CompletionTrigger trigger) {
    if (interaction_.active()) {
        return std::unexpected("completion is unavailable while the minibuffer owns focus");
    }
    if (target.window != window_id() || target.buffer != buffer_id() || target.view != view_id()) {
        return std::unexpected("completion target is not the active editor view");
    }
    EditSession& active = session_for(target.view);
    const TextOffset caret = active.caret();
    if (anchor > caret) {
        return std::unexpected("completion anchor follows the caret");
    }
    if (providers.empty()) {
        return std::unexpected("completion requires at least one provider");
    }
    CommandContext context(runtime_, target.window, target.buffer, target.view);
    return completion_->start(context, anchor, std::move(providers), std::move(trigger));
}

bool EditorApplication::move_completion(std::int64_t delta) {
    return completion_->select_relative(delta);
}

std::expected<void, std::string> EditorApplication::apply_completion(bool replace) {
    return completion_->apply({.replace = replace});
}

bool EditorApplication::cancel_completion() {
    return completion_->cancel();
}

TextInputPolicy EditorApplication::text_input_policy() const {
    return input_state().text_input;
}

const InputStateRegistry::Definition& EditorApplication::input_state() const {
    if (const InteractionMechanismState* interaction = interaction_.state()) {
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
        set_message(std::format("close policy failed: {}", command.error()));
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
        set_message(std::format("input state '{}' has no text command", state.name));
        record_command_input("text", false);
        return;
    }
    const std::optional<CommandId> command = runtime_.commands().find(*state.text_command);
    if (!command) {
        command_loop_.cancel_pending();
        set_message(std::format("unknown input text command '{}'", *state.text_command));
        record_command_input("text", false);
        return;
    }
    CommandContext context = command_context();
    CommandInvocation invocation{.arguments = {std::string(text)},
                                 .prefix = command_loop_.pending_prefix()};
    (void)handle_loop_result(command_loop_.execute(*command, context, invocation));
    record_command_input("text", false);
    sync_keymaps();
}

void EditorApplication::reset_preferred_column() {
    editing_mechanisms_.reset_preferred_column(interaction_.active() ? interaction_.state()->view
                                                                     : view_id());
}

std::expected<void, std::string> EditorApplication::open_file(std::string_view input) {
    return guile_.open_resource(window_id(), input);
}

std::expected<WindowId, std::string>
EditorApplication::display_buffer(BufferId buffer, std::string_view intent, WindowId origin,
                                  std::optional<LinePosition> position) {
    if (intent.empty()) {
        return std::unexpected("display intent must not be empty");
    }
    if (runtime_.buffers().try_get(buffer) == nullptr) {
        return std::unexpected("unknown display buffer");
    }
    const WorkbenchId workbench_id =
        workbenches_.find_by_window(origin).value_or(workbenches_.active_id());
    Workbench& workbench = workbenches_.get(workbench_id);
    if (!workbench.layout().contains(origin) || runtime_.windows().try_get(origin) == nullptr) {
        origin = workbench.active_window();
    }
    const bool replay = intent == "replay";
    const std::optional<JumpNodeId> from = replay ? std::nullopt : capture_jump(workbench, origin);
    GuileDisplayFacts facts{.intent = std::string(intent),
                            .origin = origin,
                            .active = workbench.active_window(),
                            .windows = {},
                            .slots = {}};
    for (const WindowId window : workbench.layout().leaves()) {
        const Window& candidate = runtime_.windows().get(window);
        facts.windows.push_back({.window = window,
                                 .role = candidate.role(),
                                 .pinned = candidate.pinned(),
                                 .created_by_policy = candidate.created_by_policy()});
    }
    for (const auto& [role, window] : workbench.slots()) {
        facts.slots.push_back({.role = role, .window = window});
    }
    std::ranges::sort(facts.slots, {}, &GuileDisplaySlot::role);

    const auto validate_plan = [&](const GuileDisplayPlan& plan) -> std::optional<std::string> {
        if (!workbench.layout().contains(plan.target)) {
            return "selected a window outside the active workbench";
        }
        if (plan.action == GuileDisplayPlan::Action::Reuse &&
            runtime_.windows().get(plan.target).pinned() && intent != "explicit") {
            return "selected a pinned window";
        }
        return std::nullopt;
    };
    std::expected<GuileDisplayPlan, std::string> resolved = guile_.display_plan(facts);
    std::optional<std::string> policy_error;
    if (!resolved) {
        policy_error = resolved.error();
    } else {
        policy_error = validate_plan(*resolved);
    }
    if (policy_error) {
        set_message(
            std::format("display policy failed: {}; using default Scheme policy", *policy_error));
        resolved = guile_.fallback_display_plan(facts);
        if (!resolved) {
            return std::unexpected(
                std::format("default Scheme display policy failed: {}", resolved.error()));
        }
        if (const std::optional<std::string> fallback_error = validate_plan(*resolved)) {
            return std::unexpected(
                std::format("default Scheme display policy failed: {}", *fallback_error));
        }
    }
    const GuileDisplayPlan& plan = *resolved;
    WindowId target;
    if (plan.action == GuileDisplayPlan::Action::Reuse) {
        if (!show_buffer(plan.target, buffer) || !focus_window(workbench_id, plan.target)) {
            return std::unexpected("display policy target cannot show the buffer");
        }
        target = plan.target;
    } else {
        const ViewId view = create_view({}, buffer);
        const WindowId window = runtime_.windows().create(view);
        view_state_for(view).window = window;
        if (!workbench.layout().split({.target = plan.target,
                                       .new_window = window,
                                       .axis = plan.axis,
                                       .ratio = plan.ratio})) {
            destroy_window(window);
            return std::unexpected("display policy split cannot be applied");
        }
        runtime_.windows().get(window).set_created_by_policy(true);
        if (plan.role) {
            if (std::expected<void, std::string> assigned = set_window_role(window, plan.role);
                !assigned) {
                (void)workbench.layout().erase(window);
                destroy_window(window);
                return std::unexpected(assigned.error());
            }
        }
        workbench.visit_buffer(buffer);
        if (!focus_window(workbench_id, window)) {
            return std::unexpected("display policy window cannot receive focus");
        }
        target = window;
    }
    if (position) {
        apply_position(target, *position);
    }
    const std::optional<JumpNodeId> to = capture_jump(workbench, target);
    if (from && to) {
        Window& target_window = runtime_.windows().get(target);
        (void)target_window.jump_walk().record(*from);
        (void)target_window.jump_walk().record(*to);
        if (*from != *to) {
            (void)workbench.jumps().link(*from, *to, jump_edge_kind(intent));
        }
    }
    (void)evict_jump_graph(workbench, default_jump_graph_limit);
    return target;
}

bool EditorApplication::navigate_jump(WindowId window, std::int64_t delta) {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    Window* target_window = runtime_.windows().try_get(window);
    if (!owner || target_window == nullptr) {
        return false;
    }
    JumpWalk& walk = target_window->jump_walk();
    const std::optional<std::size_t> previous = walk.cursor();
    const std::optional<JumpNodeId> target = walk.move(delta);
    if (!target) {
        return false;
    }
    if (restore_jump(workbenches_.get(*owner), window, *target)) {
        return true;
    }
    const std::optional<std::size_t> current = walk.cursor();
    if (previous && current) {
        const std::int64_t rollback =
            static_cast<std::int64_t>(*previous) - static_cast<std::int64_t>(*current);
        (void)walk.move(rollback);
    }
    return false;
}

std::vector<JumpEdge> EditorApplication::jump_branches(WindowId window, bool incoming) const {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    const Window* target = runtime_.windows().try_get(window);
    if (!owner || target == nullptr) {
        return {};
    }
    const std::optional<JumpNodeId> current = target->jump_walk().current();
    if (!current) {
        return {};
    }
    const JumpGraph& graph = workbenches_.get(*owner).jumps();
    return incoming ? graph.incoming(*current) : graph.outgoing(*current);
}

std::optional<JumpNode> EditorApplication::jump_node(WindowId window, JumpNodeId node) const {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    if (!owner) {
        return std::nullopt;
    }
    const JumpNode* found = workbenches_.get(*owner).jumps().find(node);
    return found == nullptr ? std::nullopt : std::optional(*found);
}

std::size_t EditorApplication::evict_jumps(WindowId window, std::size_t maximum_nodes) {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    return owner ? evict_jump_graph(workbenches_.get(*owner), maximum_nodes) : 0;
}

bool EditorApplication::visit_jump(WindowId window, JumpNodeId node) {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    Window* target = runtime_.windows().try_get(window);
    if (!owner || target == nullptr || !restore_jump(workbenches_.get(*owner), window, node)) {
        return false;
    }
    (void)target->jump_walk().record(node);
    return true;
}

std::optional<JumpNodeId> EditorApplication::mark_jump(WindowId window) {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    if (!owner) {
        return std::nullopt;
    }
    std::optional<JumpNodeId> node = capture_jump(workbenches_.get(*owner), window);
    if (node) {
        (void)runtime_.windows().get(window).jump_walk().record(*node);
    }
    return node;
}

bool EditorApplication::link_jump(WindowId window, JumpNodeId from, JumpNodeId to, std::string kind,
                                  bool persistent) {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    return owner && workbenches_.get(*owner).jumps().link(from, to, std::move(kind), persistent);
}

bool EditorApplication::switch_buffer(BufferId buffer) {
    return display_buffer(buffer, "explicit", window_id()).has_value();
}

bool EditorApplication::focus_window(WindowId window) {
    return focus_window(workbenches_.active_id(), window);
}

bool EditorApplication::focus_window(WorkbenchId workbench_id, WindowId window) {
    Workbench* target = workbenches_.try_get(workbench_id);
    if (target == nullptr || !target->layout().contains(window) ||
        runtime_.windows().try_get(window) == nullptr) {
        return false;
    }
    const bool active = workbench_id == workbenches_.active_id();
    Workbench& workbench = *target;
    if (active && window != workbench.active_window()) {
        command_loop_.cancel_pending();
    }
    workbench.set_active_window(window);
    workbench.visit_buffer(buffer_id(window));
    if (active) {
        show_caret();
        sync_keymaps();
    }
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
    if (const std::optional<JumpNodeId> start = capture_jump(workbench, window)) {
        (void)runtime_.windows().get(window).jump_walk().record(*start);
    }
    show_caret();
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
    show_caret();
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
    show_caret();
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
    show_caret();
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
    release_jump_anchors(*closing);
    release_location_anchors(*closing);
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
        show_caret();
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
    const auto snapshot_layout = [](this const auto& self,
                                    const WindowLayoutNode& node) -> WorkbenchLayoutSnapshot {
        if (node.leaf()) {
            return {.window = node.window,
                    .axis = WindowSplitAxis::Rows,
                    .ratio = 0.5F,
                    .children = {}};
        }
        return {.window = std::nullopt,
                .axis = node.axis,
                .ratio = node.ratio,
                .children = {self(*node.first), self(*node.second)}};
    };
    for (const WorkbenchId id : workbenches_.all()) {
        const Workbench& workbench = workbenches_.get(id);
        std::vector<std::pair<std::string, WindowId>> slots(workbench.slots().begin(),
                                                            workbench.slots().end());
        std::ranges::sort(slots, {},
                          [](const auto& slot) -> const std::string& { return slot.first; });
        result.push_back(
            {.workbench = id,
             .name = workbench.name(),
             .scope = std::vector<ProjectId>(workbench.scope().begin(), workbench.scope().end()),
             .mru = std::vector<BufferId>(workbench.mru().begin(), workbench.mru().end()),
             .windows = std::vector<WindowId>(workbench.layout().leaves().begin(),
                                              workbench.layout().leaves().end()),
             .active_window = workbench.active_window(),
             .slots = std::move(slots),
             .layout = snapshot_layout(*workbench.layout().root()),
             .active = id == workbenches_.active_id()});
    }
    return result;
}

std::vector<WorkbenchJumpSnapshot> EditorApplication::jump_graphs() const {
    std::vector<WorkbenchJumpSnapshot> result;
    for (const WorkbenchId id : workbenches_.all()) {
        const Workbench& workbench = workbenches_.get(id);
        WorkbenchJumpSnapshot snapshot{
            .workbench = id,
            .nodes = std::vector<JumpNode>(workbench.jumps().nodes().begin(),
                                           workbench.jumps().nodes().end()),
            .edges = std::vector<JumpEdge>(workbench.jumps().edges().begin(),
                                           workbench.jumps().edges().end()),
            .walks = {}};
        snapshot.walks.reserve(workbench.layout().leaves().size());
        for (const WindowId window : workbench.layout().leaves()) {
            const JumpWalk& walk = runtime_.windows().get(window).jump_walk();
            snapshot.walks.push_back(
                {.window = window,
                 .entries = std::vector<JumpNodeId>(walk.entries().begin(), walk.entries().end()),
                 .cursor = walk.cursor()});
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}

WorkbenchSessionState EditorApplication::capture_workbench_session() const {
    WorkbenchSessionState state{.version = WorkbenchSessionState::current_version,
                                .active_workbench = 0,
                                .workbenches = {}};
    const std::vector<WorkbenchId> ids = workbenches_.all();
    state.workbenches.reserve(ids.size());
    const auto capture_walk = [](const JumpWalk& walk) {
        constexpr std::size_t maximum_entries = 128;
        std::vector<JumpNodeId> entries(walk.entries().begin(), walk.entries().end());
        std::optional<std::size_t> cursor = walk.cursor();
        if (entries.size() > maximum_entries && cursor) {
            const std::size_t half = maximum_entries / 2;
            const std::size_t first = std::min(std::max<std::size_t>(*cursor, half) - half,
                                               entries.size() - maximum_entries);
            entries =
                std::vector(entries.begin() + static_cast<std::ptrdiff_t>(first),
                            entries.begin() + static_cast<std::ptrdiff_t>(first + maximum_entries));
            cursor = *cursor - first;
        }
        return std::pair{std::move(entries), cursor};
    };
    const auto capture_layout = [&](this const auto& self,
                                    const WindowLayoutNode& node) -> WorkbenchLayoutSessionState {
        if (!node.leaf()) {
            return {.window = std::nullopt,
                    .axis = node.axis,
                    .ratio = node.ratio,
                    .first = std::make_unique<WorkbenchLayoutSessionState>(self(*node.first)),
                    .second = std::make_unique<WorkbenchLayoutSessionState>(self(*node.second))};
        }
        const Window& window = runtime_.windows().get(node.window);
        const Buffer& buffer =
            runtime_.buffers().get(runtime_.views().get(window.view_id()).buffer_id());
        auto [jump_walk, jump_cursor] = capture_walk(window.jump_walk());
        return {
            .window =
                WorkbenchWindowSessionState{.resource = buffer.resource_uri(),
                                            .caret = runtime_.views().caret(window.view_id()).value,
                                            .role = window.role(),
                                            .pinned = window.pinned(),
                                            .created_by_policy = window.created_by_policy(),
                                            .jump_walk = std::move(jump_walk),
                                            .jump_cursor = jump_cursor},
            .axis = WindowSplitAxis::Rows,
            .ratio = 0.5F,
            .first = nullptr,
            .second = nullptr};
    };
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const Workbench& workbench = workbenches_.get(ids[index]);
        WorkbenchSessionEntry entry{.name = workbench.name(),
                                    .scope_roots = {},
                                    .mru_resources = {},
                                    .layout = capture_layout(*workbench.layout().root()),
                                    .active_leaf = 0,
                                    .jump_nodes = {},
                                    .jump_edges = {}};
        for (const ProjectId project : workbench.scope()) {
            const Project& definition = runtime_.projects().get(project);
            if (!definition.roots().empty()) {
                entry.scope_roots.push_back(definition.roots().front());
            }
        }
        for (const BufferId buffer : workbench.mru()) {
            const Buffer* definition = runtime_.buffers().try_get(buffer);
            if (definition != nullptr && definition->resource_uri()) {
                entry.mru_resources.push_back(*definition->resource_uri());
            }
        }
        const auto active =
            std::ranges::find(workbench.layout().leaves(), workbench.active_window());
        entry.active_leaf =
            static_cast<std::size_t>(std::distance(workbench.layout().leaves().begin(), active));
        std::set<JumpNodeId> durable_nodes;
        for (const JumpNode& node : workbench.jumps().nodes()) {
            if (node.position.resource.empty() ||
                !std::filesystem::path(node.position.resource).is_absolute()) {
                continue;
            }
            durable_nodes.insert(node.id);
            entry.jump_nodes.push_back({.id = node.id,
                                        .resource = node.position.resource,
                                        .fallback = node.position.fallback,
                                        .excerpt = node.position.excerpt,
                                        .created_at = node.created_at,
                                        .last_visit = node.last_visit});
        }
        for (const JumpEdge& edge : workbench.jumps().edges()) {
            if (durable_nodes.contains(edge.from) && durable_nodes.contains(edge.to)) {
                entry.jump_edges.push_back({.from = edge.from,
                                            .to = edge.to,
                                            .kind = edge.kind,
                                            .at = edge.at,
                                            .persistent = edge.persistent});
            }
        }
        const auto filter_walk = [&](this const auto& self,
                                     WorkbenchLayoutSessionState& node) -> void {
            if (!node.leaf()) {
                self(*node.first);
                self(*node.second);
                return;
            }
            WorkbenchWindowSessionState& window = *node.window;
            JumpWalk filtered;
            if (window.jump_cursor) {
                filtered.restore(std::move(window.jump_walk), window.jump_cursor);
                std::vector<JumpNodeId> transient;
                for (const JumpNodeId jump : filtered.entries()) {
                    if (!durable_nodes.contains(jump)) {
                        transient.push_back(jump);
                    }
                }
                filtered.forget(transient);
            }
            window.jump_walk.assign(filtered.entries().begin(), filtered.entries().end());
            window.jump_cursor = filtered.cursor();
        };
        filter_walk(entry.layout);
        if (ids[index] == workbenches_.active_id()) {
            state.active_workbench = index;
        }
        state.workbenches.push_back(std::move(entry));
    }
    return state;
}

std::string EditorApplication::serialize_workbench_session() const {
    return cind::serialize_workbench_session(capture_workbench_session());
}

std::expected<void, std::string>
EditorApplication::restore_workbench_session(std::string_view serialized) {
    return guile_.restore_workbench_session(serialized);
}

std::expected<GuileWorkbenchRestorePlan, std::string>
EditorApplication::prepare_workbench_session_restore(std::string_view serialized) {
    std::expected<WorkbenchSessionState, std::string> parsed = parse_workbench_session(serialized);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto has_duplicate = [](const std::vector<std::string>& values) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (std::ranges::find(values.begin() + static_cast<std::ptrdiff_t>(index + 1),
                                  values.end(), values[index]) != values.end()) {
                return true;
            }
        }
        return false;
    };
    const auto stable_path = [](const std::string& value) {
        return !value.empty() && value.find('\0') == std::string::npos &&
               std::filesystem::path(value).is_absolute();
    };
    std::vector<std::string> names;
    names.reserve(parsed->workbenches.size());
    for (const WorkbenchSessionEntry& entry : parsed->workbenches) {
        if (std::ranges::find(names, entry.name) != names.end()) {
            return std::unexpected("workbench session contains a duplicate name");
        }
        if (has_duplicate(entry.scope_roots) ||
            std::ranges::any_of(entry.scope_roots,
                                [&](const std::string& root) { return !stable_path(root); })) {
            return std::unexpected("workbench session contains an invalid project scope");
        }
        if (has_duplicate(entry.mru_resources) ||
            std::ranges::any_of(entry.mru_resources, [&](const std::string& resource) {
                return !stable_path(resource);
            })) {
            return std::unexpected("workbench session contains an invalid buffer MRU");
        }
        names.push_back(entry.name);
        std::set<JumpNodeId> jump_nodes;
        for (const WorkbenchJumpNodeSessionState& node : entry.jump_nodes) {
            if (node.id == kInvalidJumpNode || !stable_path(node.resource) ||
                !jump_nodes.insert(node.id).second) {
                return std::unexpected("workbench session contains an invalid jump node");
            }
        }
        if (std::ranges::any_of(entry.jump_edges, [&](const WorkbenchJumpEdgeSessionState& edge) {
                return edge.kind.empty() || !jump_nodes.contains(edge.from) ||
                       !jump_nodes.contains(edge.to);
            })) {
            return std::unexpected("workbench session contains an invalid jump edge");
        }
        std::vector<std::string> roles;
        const auto validate_layout = [&](this const auto& self,
                                         const WorkbenchLayoutSessionState& node)
            -> std::expected<std::size_t, std::string> {
            if (node.leaf()) {
                const WorkbenchWindowSessionState& window = *node.window;
                if (window.resource && !stable_path(*window.resource)) {
                    return std::unexpected("workbench session contains an invalid window resource");
                }
                if (window.role) {
                    if (window.role->empty() ||
                        std::ranges::find(roles, *window.role) != roles.end()) {
                        return std::unexpected(
                            "workbench session contains an invalid or duplicate window role");
                    }
                    roles.push_back(*window.role);
                }
                if ((window.jump_walk.empty() && window.jump_cursor) ||
                    (!window.jump_walk.empty() && !window.jump_cursor) ||
                    (window.jump_cursor && *window.jump_cursor >= window.jump_walk.size()) ||
                    std::ranges::any_of(window.jump_walk, [&](JumpNodeId jump) {
                        return !jump_nodes.contains(jump);
                    })) {
                    return std::unexpected("workbench session contains an invalid jump walk");
                }
                return 1;
            }
            const std::expected<std::size_t, std::string> first = self(*node.first);
            if (!first) {
                return std::unexpected(first.error());
            }
            const std::expected<std::size_t, std::string> second = self(*node.second);
            if (!second) {
                return std::unexpected(second.error());
            }
            return *first + *second;
        };
        const std::expected<std::size_t, std::string> leaves = validate_layout(entry.layout);
        if (!leaves) {
            return std::unexpected(leaves.error());
        }
        if (*leaves == 0 || entry.active_leaf >= *leaves) {
            return std::unexpected("workbench session contains an invalid active window");
        }
    }

    std::map<std::string, std::vector<GuileWorkbenchRestoreTarget>> resources;
    std::vector<GuileWorkbenchRestoreMru> mru;
    const BufferId fallback = buffer_id();
    const std::vector<WorkbenchId> previous = workbenches_.all();
    const WorkbenchId previous_active = workbenches_.active_id();
    std::vector<WorkbenchId> created;
    created.reserve(parsed->workbenches.size());
    std::vector<ProjectId> created_projects;

    const auto project_for_root = [&](const std::string& root) -> ProjectId {
        if (const std::optional<ProjectId> existing = runtime_.projects().find_by_root(root)) {
            return *existing;
        }
        std::string name = std::filesystem::path(root).filename().string();
        if (name.empty()) {
            name = root;
        }
        const ProjectId project = runtime_.projects().create({.name = std::move(name),
                                                              .roots = {root},
                                                              .discovery_provider = "session",
                                                              .discovery_marker = {}});
        created_projects.push_back(project);
        return project;
    };
    try {
        for (std::size_t index = 0; index < parsed->workbenches.size(); ++index) {
            const WorkbenchSessionEntry& source = parsed->workbenches[index];
            std::vector<ProjectId> scope;
            scope.reserve(source.scope_roots.size());
            for (const std::string& root : source.scope_roots) {
                scope.push_back(project_for_root(root));
            }
            const ViewId root_view = create_view({}, fallback);
            const WindowId root_window = runtime_.windows().create(root_view);
            view_state_for(root_view).window = root_window;
            std::string temporary = std::format(" *restore-{}*", index);
            while (workbenches_.find_by_name(temporary) ||
                   std::ranges::find(names, temporary) != names.end()) {
                temporary.push_back('*');
            }
            const WorkbenchId id = workbenches_.create({.name = std::move(temporary),
                                                        .root_window = root_window,
                                                        .scope = std::move(scope)});
            created.push_back(id);
            Workbench& workbench = workbenches_.get(id);
            std::vector<JumpNode> restored_nodes;
            restored_nodes.reserve(source.jump_nodes.size());
            for (const WorkbenchJumpNodeSessionState& node : source.jump_nodes) {
                restored_nodes.push_back({.id = node.id,
                                          .position = {.buffer = {},
                                                       .resource = node.resource,
                                                       .anchor = 0,
                                                       .fallback = node.fallback,
                                                       .excerpt = node.excerpt},
                                          .created_at = node.created_at,
                                          .last_visit = node.last_visit});
            }
            std::vector<JumpEdge> restored_edges;
            restored_edges.reserve(source.jump_edges.size());
            for (const WorkbenchJumpEdgeSessionState& edge : source.jump_edges) {
                restored_edges.push_back({.from = edge.from,
                                          .to = edge.to,
                                          .kind = edge.kind,
                                          .at = edge.at,
                                          .persistent = edge.persistent});
            }
            workbench.jumps().restore(std::move(restored_nodes), std::move(restored_edges));
            for (const std::unique_ptr<BufferState>& buffer : buffers_) {
                resolve_jump_nodes(buffer->buffer);
            }
            const auto apply_node = [&](this const auto& self,
                                        const WorkbenchLayoutSessionState& node,
                                        WindowId target) -> void {
                if (!node.leaf()) {
                    const ViewId second_view = create_view({}, fallback);
                    const WindowId second_window = runtime_.windows().create(second_view);
                    view_state_for(second_view).window = second_window;
                    if (!workbench.layout().split({.target = target,
                                                   .new_window = second_window,
                                                   .axis = node.axis,
                                                   .ratio = node.ratio})) {
                        destroy_window(second_window);
                        throw std::runtime_error("cannot restore workbench layout split");
                    }
                    self(*node.first, target);
                    self(*node.second, second_window);
                    return;
                }
                const WorkbenchWindowSessionState& leaf = *node.window;
                BufferId displayed = fallback;
                if (leaf.resource) {
                    if (const std::optional<BufferId> existing =
                            runtime_.buffers().find_by_resource(*leaf.resource)) {
                        displayed = *existing;
                    } else {
                        resources[*leaf.resource].push_back(
                            {.window = target, .caret = leaf.caret});
                    }
                }
                if (!show_buffer(target, displayed)) {
                    throw std::runtime_error("cannot restore workbench window buffer");
                }
                const TextOffset end =
                    runtime_.buffers().get(displayed).snapshot().content().end_offset();
                runtime_.views().set_caret(runtime_.windows().get(target).view_id(),
                                           TextOffset{std::min(leaf.caret, end.value)});
                if (std::expected<void, std::string> role = set_window_role(target, leaf.role);
                    !role) {
                    throw std::runtime_error(role.error());
                }
                runtime_.windows().get(target).set_pinned(leaf.pinned);
                runtime_.windows().get(target).set_created_by_policy(leaf.created_by_policy);
                runtime_.windows().get(target).jump_walk().restore(leaf.jump_walk,
                                                                   leaf.jump_cursor);
            };
            apply_node(source.layout, root_window);
            const std::vector<WindowId> leaves(workbench.layout().leaves().begin(),
                                               workbench.layout().leaves().end());
            workbench.set_active_window(leaves[source.active_leaf]);
            for (const std::string& resource : source.mru_resources) {
                if (!runtime_.buffers().find_by_resource(resource)) {
                    (void)resources[resource];
                }
            }
            mru.push_back({.workbench = id, .resources = source.mru_resources, .windows = leaves});
        }
    } catch (const std::exception& exception) {
        (void)workbenches_.activate(previous_active);
        for (const WorkbenchId id : created) {
            if (Workbench* workbench = workbenches_.try_get(id)) {
                const std::vector<WindowId> windows(workbench->layout().leaves().begin(),
                                                    workbench->layout().leaves().end());
                release_jump_anchors(*workbench);
                (void)workbenches_.erase(id);
                for (const WindowId window : windows) {
                    destroy_window(window);
                }
            }
        }
        for (const ProjectId project : created_projects) {
            (void)runtime_.projects().erase(project);
        }
        return std::unexpected(exception.what());
    }

    const WorkbenchId selected = created[parsed->active_workbench];
    (void)workbenches_.activate(selected);
    for (const WorkbenchId id : previous) {
        Workbench& old = workbenches_.get(id);
        const std::vector<WindowId> windows(old.layout().leaves().begin(),
                                            old.layout().leaves().end());
        release_jump_anchors(old);
        (void)workbenches_.erase(id);
        for (const WindowId window : windows) {
            destroy_window(window);
        }
    }
    for (std::size_t index = 0; index < created.size(); ++index) {
        if (!workbenches_.rename(created[index], parsed->workbenches[index].name)) {
            return std::unexpected("cannot restore workbench name");
        }
    }
    show_caret();
    sync_keymaps();
    GuileWorkbenchRestorePlan plan{.resources = {}, .mru = std::move(mru)};
    plan.resources.reserve(resources.size());
    for (auto& [resource, targets] : resources) {
        plan.resources.push_back({.resource = resource, .targets = std::move(targets)});
    }
    return plan;
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
    if (buffer == replacement || runtime_.buffers().try_get(replacement) == nullptr ||
        std::ranges::none_of(buffers_, [replacement](const std::unique_ptr<BufferState>& state) {
            return state->buffer == replacement;
        })) {
        return std::unexpected("replacement buffer is not open");
    }

    Buffer& closing_buffer = runtime_.buffers().get(buffer);
    const Text closing_text = closing_buffer.snapshot().content();
    for (const WorkbenchId workbench : workbenches_.all()) {
        workbenches_.get(workbench).jumps().detach_buffer(
            buffer,
            [&](AnchorId anchor) {
                return closing_text.position(closing_buffer.navigation_anchor_offset(anchor));
            },
            [&](AnchorId anchor) { closing_buffer.remove_navigation_anchor(anchor); });
    }

    for (const WorkbenchId workbench : workbenches_.all()) {
        workbenches_.get(workbench).location_lists().detach_buffer(
            buffer,
            [&](AnchorId anchor) {
                return closing_text.position(closing_buffer.navigation_anchor_offset(anchor));
            },
            [&](AnchorId anchor) { closing_buffer.remove_navigation_anchor(anchor); });
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
    if (const Buffer* released = runtime_.buffers().try_get(buffer);
        released != nullptr && released->resource_uri()) {
        (void)lsp_sessions_->close_document(path_to_file_uri(*released->resource_uri()));
    }
    buffers_.erase(found);
    if (!runtime_.buffers().erase(buffer)) {
        throw std::logic_error("buffer lifecycle registry is inconsistent");
    }
    workbenches_.forget_buffer(buffer);
    if (const std::expected<void, std::string> released = guile_.lsp_buffer_released(buffer);
        !released) {
        set_message(std::format("LSP binding release failed: {}", released.error()));
    }
    show_caret();
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
        const std::vector<Diagnostic> diagnostics = buffer.diagnostics();
        result.push_back(
            {.buffer = state.buffer,
             .view = view != nullptr ? std::optional(view->view) : std::nullopt,
             .name = buffer.name(),
             .resource = buffer.resource_uri(),
             .modified = buffer.modified(),
             .active = state.buffer == buffer_id(),
             .saving = guile_.buffer_saving(state.buffer).value_or(false),
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
             .completion_auto = mode_policy.completion_auto,
             .things = mode_policy.things,
             .completion_providers = mode_policy.completion_providers,
             .location_count = buffer.locations().size(),
             .diagnostic_count = diagnostics.size(),
             .diagnostic_errors = static_cast<std::size_t>(
                 std::ranges::count(diagnostics, DiagnosticSeverity::Error, &Diagnostic::severity)),
             .diagnostic_warnings = static_cast<std::size_t>(std::ranges::count(
                 diagnostics, DiagnosticSeverity::Warning, &Diagnostic::severity))});
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
    const LocationList* list = active_workbench().location_lists().current();
    if (list == nullptr) {
        return {};
    }
    std::optional<std::size_t> selected_index = list->selected;
    if (selected_index && *selected_index >= list->items.size()) {
        selected_index.reset();
    }
    return {.buffer = list->materialized_buffer,
            .selected_index = selected_index,
            .location_count = list->items.size()};
}

std::vector<LocationListSnapshot> EditorApplication::location_lists(WorkbenchId workbench) const {
    const Workbench* target = workbenches_.try_get(workbench);
    if (target == nullptr) {
        return {};
    }
    const LocationList* current = target->location_lists().current();
    std::vector<LocationListSnapshot> result;
    result.reserve(target->location_lists().lists().size());
    for (const LocationList& list : target->location_lists().lists()) {
        result.push_back({.list = list.id,
                          .source = list.source,
                          .materialized_buffer = list.materialized_buffer,
                          .selected_index = list.selected,
                          .item_count = list.items.size(),
                          .version = list.version,
                          .current = current != nullptr && current->id == list.id});
    }
    return result;
}

bool EditorApplication::move_location_list(int delta) {
    return active_workbench().location_lists().move(delta);
}

TransactionGroupId
EditorApplication::record_transaction_group(std::string source,
                                            std::vector<TransactionGroupEntry> entries) {
    return active_workbench().transaction_groups().record(std::move(source), std::move(entries));
}

std::optional<TransactionGroupResult>
EditorApplication::move_transaction_group(TransactionGroupId group, bool redo) {
    auto current = [&](BufferId buffer) -> std::optional<UndoNodeId> {
        const Buffer* target = runtime_.buffers().try_get(buffer);
        return target != nullptr ? std::optional(target->undo_position()) : std::nullopt;
    };
    auto navigate = [&](BufferId buffer, UndoNodeId position) {
        auto found = std::ranges::find_if(views_, [buffer](const std::unique_ptr<ViewState>& view) {
            return view->buffer == buffer;
        });
        if (found == views_.end()) {
            const ViewId view = create_view({}, buffer);
            found =
                std::ranges::find_if(views_, [view](const std::unique_ptr<ViewState>& candidate) {
                    return candidate->view == view;
                });
        }
        return found != views_.end() && (*found)->session->undo_to(position);
    };
    TransactionGroupRegistry& groups = active_workbench().transaction_groups();
    return redo ? groups.redo(group, current, navigate) : groups.undo(group, current, navigate);
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
    return runtime_.buffers().get(buffer_id()).save_generation();
}

std::uint32_t EditorApplication::save_generation(WindowId window) const {
    return runtime_.buffers().get(buffer_id(window)).save_generation();
}

bool EditorApplication::has_background_work() const {
    return async_runtime_.has_work();
}

bool EditorApplication::poll_background_work() {
    return async_runtime_.drain() != 0;
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
        resolve_location_lists(buffer);
        resolve_jump_nodes(buffer);
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
    show_caret();
    if (window == window_id()) {
        sync_keymaps();
    }
    return true;
}

std::expected<WindowId, std::string>
EditorApplication::display_generated_buffer(WindowId origin, std::string name, std::string text,
                                            ModeId mode, std::string style_origin,
                                            std::string_view intent) {
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
        const std::expected<WindowId, std::string> displayed =
            display_buffer(buffer, intent, origin, LinePosition{});
        if (!displayed) {
            return std::unexpected(displayed.error());
        }
        ViewState* view = find_view(*displayed, buffer);
        if (view == nullptr) {
            return std::unexpected("generated buffer view was not created");
        }
        runtime_.views().clear_selection(view->view);
        runtime_.views().get(view->view).viewport() = {};
        editing_mechanisms_.reset_preferred_column(view->view);
        return *displayed;
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
        show_caret();
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

std::expected<void, std::string>
EditorApplication::publish_location_list(WindowId window, BufferId buffer, std::string source,
                                         std::vector<BufferLocation> locations) {
    const std::optional<WorkbenchId> owner = workbenches_.find_by_window(window);
    if (!owner || runtime_.buffers().try_get(buffer) == nullptr) {
        return std::unexpected("location list display target is unavailable");
    }
    try {
        runtime_.buffers().set_locations(buffer, locations);
        std::vector<LocationItem> items;
        items.reserve(locations.size());
        for (BufferLocation& location : locations) {
            items.push_back({.resource = std::move(location.resource),
                             .range = {.start = location.target, .end = location.target},
                             .excerpt = std::move(location.excerpt),
                             .metadata = {},
                             .resolved = std::nullopt});
        }
        Workbench& workbench = workbenches_.get(*owner);
        (void)workbench.location_lists().publish(std::move(source), std::move(items), buffer);
        for (const std::unique_ptr<BufferState>& state : buffers_) {
            const Buffer& candidate = runtime_.buffers().get(state->buffer);
            if (candidate.resource_uri()) {
                resolve_location_lists(state->buffer);
            }
        }
        return {};
    } catch (const std::exception& exception) {
        return std::unexpected(exception.what());
    }
}

std::optional<LocationItem> EditorApplication::location_item(std::optional<BufferId> materialized,
                                                             std::size_t index) const {
    if (!materialized) {
        const LocationList* list = active_workbench().location_lists().current();
        return list != nullptr && index < list->items.size()
                   ? std::optional<LocationItem>{list->items[index]}
                   : std::nullopt;
    }
    for (const WorkbenchId workbench : workbenches_.all()) {
        const LocationList* list =
            workbenches_.get(workbench).location_lists().find_by_buffer(*materialized);
        if (list == nullptr || index >= list->items.size()) {
            continue;
        }
        return list->items[index];
    }
    return std::nullopt;
}

void EditorApplication::resolve_location_lists(BufferId buffer_id) {
    Buffer& buffer = runtime_.buffers().get(buffer_id);
    if (!buffer.resource_uri()) {
        return;
    }
    for (const WorkbenchId workbench : workbenches_.all()) {
        workbenches_.get(workbench).location_lists().resolve_resource(
            *buffer.resource_uri(),
            [&](const LocationItem& item) { return resolve_location(buffer, item); });
    }
}

void EditorApplication::resolve_jump_nodes(BufferId buffer_id) {
    Buffer& buffer = runtime_.buffers().get(buffer_id);
    if (!buffer.resource_uri()) {
        return;
    }
    const DocumentSnapshot snapshot = buffer.snapshot();
    const Text& text = snapshot.content();
    for (const WorkbenchId workbench : workbenches_.all()) {
        workbenches_.get(workbench).jumps().attach_buffer(
            *buffer.resource_uri(), buffer_id, [&](const JumpPosition& position) {
                std::uint32_t line = std::min(position.fallback.line, text.line_count() - 1);
                const auto matches = [&](std::uint32_t candidate) {
                    return position.excerpt.empty() ||
                           text.substring(text.line_content_range(candidate)) == position.excerpt;
                };
                if (!matches(line) && !position.excerpt.empty()) {
                    constexpr std::uint32_t radius = 64;
                    const std::uint32_t first = line > radius ? line - radius : 0;
                    const std::uint32_t last = std::min(text.line_count() - 1, line + radius);
                    std::optional<std::uint32_t> nearby;
                    for (std::uint32_t candidate = first; candidate <= last; ++candidate) {
                        if (!matches(candidate)) {
                            continue;
                        }
                        if (nearby) {
                            nearby.reset();
                            break;
                        }
                        nearby = candidate;
                    }
                    if (nearby) {
                        line = *nearby;
                    }
                }
                LinePosition fallback{.line = line,
                                      .byte_column =
                                          std::min(position.fallback.byte_column,
                                                   text.line_content_range(line).length())};
                return std::pair{buffer.create_navigation_anchor(text.offset(fallback)), fallback};
            });
    }
}

ResolvedLocation EditorApplication::resolve_location(Buffer& buffer, const LocationItem& item) {
    const DocumentSnapshot snapshot = buffer.snapshot();
    const Text& text = snapshot.content();
    const std::uint32_t fallback_line = std::min(item.range.start.line, text.line_count() - 1);
    std::uint32_t target_line = fallback_line;
    bool stale = false;
    const auto line_matches = [&](std::uint32_t line) {
        return item.excerpt.empty() ||
               text.substring(text.line_content_range(line)) == item.excerpt;
    };
    if (!line_matches(target_line)) {
        std::vector<std::uint32_t> matches;
        const std::uint32_t begin = target_line > 20 ? target_line - 20 : 0;
        const std::uint32_t end = std::min(text.line_count() - 1, target_line + 20);
        for (std::uint32_t line = begin; line <= end; ++line) {
            if (line_matches(line)) {
                matches.push_back(line);
            }
        }
        if (matches.size() != 1) {
            matches.clear();
            for (std::uint32_t line = 0; line < text.line_count(); ++line) {
                if (line_matches(line)) {
                    matches.push_back(line);
                    if (matches.size() > 1) {
                        break;
                    }
                }
            }
        }
        if (matches.size() == 1) {
            target_line = matches.front();
        } else {
            stale = true;
        }
    }
    EncodedLinePosition encoded = item.range.start;
    encoded.line = target_line;
    const LinePosition position =
        resolve_line_position(text, encoded)
            .value_or(LinePosition{.line = target_line,
                                   .byte_column = text.line_content_range(target_line).length()});
    const AnchorId anchor = buffer.create_navigation_anchor(text.offset(position));
    return {.buffer = buffer.id(), .start = anchor, .end = anchor, .stale = stale};
}

void EditorApplication::release_location_anchors(Workbench& workbench) {
    workbench.location_lists().release_anchors([&](BufferId buffer, AnchorId anchor) {
        if (Buffer* target = runtime_.buffers().try_get(buffer)) {
            target->remove_navigation_anchor(anchor);
        }
    });
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
    show_caret();
}

std::optional<JumpNodeId> EditorApplication::capture_jump(Workbench& workbench, WindowId window) {
    Window* target_window = runtime_.windows().try_get(window);
    if (target_window == nullptr || !workbench.layout().contains(window)) {
        return std::nullopt;
    }
    const ViewId view = target_window->view_id();
    const BufferId buffer_id = runtime_.views().get(view).buffer_id();
    Buffer& buffer = runtime_.buffers().get(buffer_id);
    const EditSession& target = session_for(view);
    const DocumentSnapshot snapshot = target.snapshot();
    const TextOffset caret = target.caret();
    const LinePosition fallback = snapshot.content().position(caret);
    const AnchorId anchor = buffer.create_navigation_anchor(caret);
    JumpInternResult result =
        workbench.jumps().intern({.buffer = buffer_id,
                                  .resource = buffer.resource_uri().value_or(std::string()),
                                  .anchor = anchor,
                                  .fallback = fallback,
                                  .excerpt = snapshot.content().substring(
                                      snapshot.content().line_content_range(fallback.line))});
    if (!result.retained_position) {
        buffer.remove_navigation_anchor(anchor);
    }
    return result.node;
}

bool EditorApplication::restore_jump(Workbench& workbench, WindowId window, JumpNodeId node) {
    JumpNode* target = workbench.jumps().find(node);
    if (target == nullptr) {
        return false;
    }
    if (!target->position.buffer ||
        runtime_.buffers().try_get(target->position.buffer) == nullptr) {
        if (target->position.resource.empty()) {
            return false;
        }
        (void)workbench.jumps().touch(node);
        const std::expected<void, std::string> opened =
            guile_.open_resource(window, target->position.resource, target->position.fallback.line,
                                 target->position.fallback.byte_column, "replay");
        if (!opened) {
            set_message(std::format("jump reopen failed: {}", opened.error()));
            return false;
        }
        return true;
    }
    Buffer& buffer = runtime_.buffers().get(target->position.buffer);
    LinePosition position = target->position.fallback;
    if (target->position.anchor != 0) {
        position = buffer.snapshot().content().position(
            buffer.navigation_anchor_offset(target->position.anchor));
        target->position.fallback = position;
    }
    if (!show_buffer(window, target->position.buffer) || !focus_window(workbench.id(), window)) {
        return false;
    }
    apply_position(window, position);
    (void)workbench.jumps().touch(node);
    return true;
}

std::size_t EditorApplication::evict_jump_graph(Workbench& workbench, std::size_t maximum_nodes) {
    std::vector<JumpNode> removed = workbench.jumps().evict(maximum_nodes);
    if (removed.empty()) {
        return 0;
    }
    std::vector<JumpNodeId> ids;
    ids.reserve(removed.size());
    for (const JumpNode& node : removed) {
        ids.push_back(node.id);
        if (node.position.buffer && node.position.anchor != 0) {
            if (Buffer* buffer = runtime_.buffers().try_get(node.position.buffer)) {
                buffer->remove_navigation_anchor(node.position.anchor);
            }
        }
    }
    for (const WindowId window : workbench.layout().leaves()) {
        if (Window* target = runtime_.windows().try_get(window)) {
            target->jump_walk().forget(ids);
        }
    }
    return removed.size();
}

void EditorApplication::release_jump_anchors(Workbench& workbench) {
    workbench.jumps().release_anchors([&](BufferId buffer, AnchorId anchor) {
        if (Buffer* target = runtime_.buffers().try_get(buffer)) {
            target->remove_navigation_anchor(anchor);
        }
    });
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
        set_message(std::format("keymap policy failed: {}", policy.error()));
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

void EditorApplication::record_command_input(std::string_view key, bool clear_message) {
    if (const std::expected<void, std::string> recorded = guile_.command_input(key, clear_message);
        !recorded) {
        return;
    }
}

bool EditorApplication::handle_loop_result(CommandLoopResult result) {
    std::optional<std::string_view> command;
    if (result.command) {
        command = runtime_.commands().definition(*result.command).name;
    }
    bool interaction_started = false;
    if (result.interaction) {
        CommandContext context = command_context();
        interaction_session_.reset();
        std::expected<void, std::string> started = interaction_.start(*result.interaction, context);
        if (!started) {
            set_message(started.error());
        } else {
            const InteractionMechanismState& state = *interaction_.state();
            if (std::expected<void, std::string> registered =
                    guile_.interaction_started(*result.interaction, state.origin);
                !registered) {
                (void)interaction_.cancel();
                set_message(registered.error());
                return result.consumed;
            }
            interaction_session_ =
                std::make_unique<EditSession>(runtime_, state.buffer, state.view, CppIndentStyle{});
            interaction_started = true;
        }
    }
    if (const std::expected<void, std::string> recorded = guile_.command_result_feedback(
            result.status, result.consumed, command, interaction_started, result.message);
        !recorded) {
        return result.consumed;
    }
    return result.consumed;
}

CommandContext EditorApplication::command_context() {
    if (const InteractionMechanismState* interaction = interaction_.state()) {
        return CommandContext(runtime_, interaction->window, interaction->buffer,
                              interaction->view);
    }
    return CommandContext(runtime_, window_id(), buffer_id(), view_id());
}

std::expected<void, std::string> EditorApplication::refresh_completion() {
    if (!completion_->active()) {
        return {};
    }
    const CompletionRequest request = completion_->state()->request;
    if (request.target.window != window_id() || request.target.buffer != buffer_id() ||
        request.target.view != view_id()) {
        (void)completion_->cancel();
        return {};
    }
    CommandContext context(runtime_, request.target.window, request.target.buffer,
                           request.target.view);
    if (runtime_.buffers().get(request.target.buffer).snapshot().revision() != request.revision) {
        return completion_->update(context);
    } else {
        (void)completion_->invalidate_if_stale();
    }
    return {};
}

std::expected<std::uint64_t, std::string>
EditorApplication::ensure_lsp_session(CommandTarget target, ScriptLspProviderSpec provider) {
    const Buffer* buffer = runtime_.buffers().try_get(target.buffer);
    const View* view = runtime_.views().try_get(target.view);
    if (buffer == nullptr || view == nullptr || view->buffer_id() != target.buffer ||
        runtime_.windows().try_get(target.window) == nullptr || !buffer->resource_uri()) {
        return std::unexpected("LSP provider requires a file-backed buffer");
    }
    std::vector<std::string> capabilities;
    capabilities.reserve(provider.features.size());
    for (const std::string& feature : provider.features) {
        if (feature == "completion") {
            capabilities.push_back(LspCompletionFeature::client_capabilities());
        } else if (feature == "diagnostics") {
            capabilities.push_back(LspDiagnosticsFeature::client_capabilities());
        } else if (feature == "navigation") {
            capabilities.push_back(LspNavigationFeature::client_capabilities());
        } else {
            return std::unexpected(
                std::format("LSP provider '{}' has unknown feature '{}'", provider.name, feature));
        }
    }
    std::expected<LspSessionId, std::string> session = lsp_sessions_->ensure(
        buffer->project_id(), {.command = provider.command,
                               .arguments = provider.arguments,
                               .root = provider.root,
                               .language_id = provider.language_id,
                               .client_capabilities = std::move(capabilities)});
    if (!session) {
        return std::unexpected(std::move(session.error()));
    }
    return session->value;
}

std::expected<void, std::string>
EditorApplication::attach_lsp_diagnostics(LspSessionId session_id) {
    LspSession* session = lsp_sessions_->find(session_id);
    if (session == nullptr) {
        return std::unexpected("unknown LSP diagnostics session");
    }
    LspDiagnosticsFeature::attach(
        *session,
        [this, session_id](LspPublishedDiagnostics published) {
            publish_lsp_diagnostics(session_id, std::move(published));
        },
        [this](const std::string& error) { report_lsp_diagnostics_failure(error); });
    return {};
}

void EditorApplication::report_lsp_diagnostics_failure(std::string_view message) {
    const std::expected<void, std::string> reported = guile_.lsp_diagnostics_failed(message);
    if (!reported) {
        return;
    }
}

void EditorApplication::publish_lsp_diagnostics(LspSessionId session_id,
                                                LspPublishedDiagnostics published) {
    const std::expected<std::string, std::string> resource = file_uri_to_path(published.uri);
    if (!resource) {
        report_lsp_diagnostics_failure(resource.error());
        return;
    }
    const std::optional<BufferId> buffer_id = runtime_.buffers().find_by_resource(*resource);
    if (!buffer_id) {
        return;
    }
    if (!guile_.lsp_session_bound(*buffer_id, session_id.value).value_or(false)) {
        return;
    }
    const DocumentSnapshot snapshot = runtime_.buffers().get(*buffer_id).snapshot();
    if (published.version && *published.version != snapshot.revision()) {
        return;
    }
    std::vector<Diagnostic> diagnostics;
    diagnostics.reserve(published.diagnostics.size());
    for (LspDiagnostic& source : published.diagnostics) {
        const std::optional<TextRange> range =
            text_range_from_lsp(snapshot.content(), source.range);
        if (!range) {
            report_lsp_diagnostics_failure("server returned an invalid range");
            return;
        }
        diagnostics.push_back({.range = *range,
                               .severity = source.severity,
                               .message = std::move(source.message),
                               .source = std::move(source.source),
                               .code = std::move(source.code)});
    }
    try {
        runtime_.buffers().set_diagnostics(*buffer_id, std::format("lsp:{}", session_id.value),
                                           snapshot.revision(), std::move(diagnostics));
    } catch (const std::exception& exception) {
        report_lsp_diagnostics_failure(exception.what());
    }
}

std::expected<void, std::string>
EditorApplication::synchronize_lsp_session(BufferId buffer_id, LspSessionId session_id) {
    Buffer* buffer = runtime_.buffers().try_get(buffer_id);
    LspSession* session = lsp_sessions_->find(session_id);
    if (buffer == nullptr || session == nullptr || !buffer->resource_uri()) {
        return std::unexpected("LSP synchronization target is unavailable");
    }
    const DocumentSnapshot snapshot = buffer->snapshot();
    if (std::expected<void, std::string> synchronized =
            session->synchronize_document({.uri = path_to_file_uri(*buffer->resource_uri()),
                                           .language_id = session->config().language_id,
                                           .revision = snapshot.revision(),
                                           .text = snapshot.content()});
        !synchronized) {
        return std::unexpected(std::move(synchronized.error()));
    }
    return {};
}

std::expected<ScriptAsyncExternalServices::Cancel, std::string>
EditorApplication::start_lsp_navigation(const ScriptLspNavigationRequest& request,
                                        ScriptExternalAsyncCallbacks callbacks) {
    LspNavigationKind kind;
    if (request.kind == "definition") {
        kind = LspNavigationKind::Definition;
    } else if (request.kind == "declaration") {
        kind = LspNavigationKind::Declaration;
    } else if (request.kind == "implementation") {
        kind = LspNavigationKind::Implementation;
    } else if (request.kind == "references") {
        kind = LspNavigationKind::References;
    } else {
        return std::unexpected(std::format("unknown LSP navigation kind '{}'", request.kind));
    }
    const CommandTarget target = request.target;
    const Buffer* buffer = runtime_.buffers().try_get(target.buffer);
    const View* view = runtime_.views().try_get(target.view);
    if (buffer == nullptr || view == nullptr || view->buffer_id() != target.buffer ||
        runtime_.windows().try_get(target.window) == nullptr || !buffer->resource_uri()) {
        return std::unexpected("LSP navigation target is unavailable");
    }
    LspSession* lsp = lsp_sessions_->find(LspSessionId{request.session});
    if (lsp == nullptr) {
        return std::unexpected("LSP navigation session is unavailable");
    }
    const DocumentSnapshot snapshot = buffer->snapshot();
    std::expected<LspSession::Cancel, std::string> started = LspNavigationFeature::request(
        *lsp, kind,
        {.uri = path_to_file_uri(*buffer->resource_uri()),
         .language_id = lsp->config().language_id,
         .revision = snapshot.revision(),
         .text = snapshot.content(),
         .caret = runtime_.views().caret(target.view),
         .include_declaration = true},
        [completed = std::move(callbacks.completed)](std::vector<LspLocation> locations) mutable {
            std::vector<ScriptLspLocation> values;
            values.reserve(locations.size());
            for (LspLocation& location : locations) {
                values.push_back({.resource = std::move(location.resource),
                                  .start_line = location.range.start.line,
                                  .start_column = location.range.start.character,
                                  .end_line = location.range.end.line,
                                  .end_column = location.range.end.character});
            }
            if (completed) {
                completed(ScriptLspNavigationResult{.locations = std::move(values)});
            }
        },
        [failed = std::move(callbacks.failed)](std::string error) mutable {
            if (failed) {
                failed(std::move(error));
            }
        },
        [cancelled = std::move(callbacks.cancelled)]() mutable {
            if (cancelled) {
                cancelled();
            }
        });
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    return std::move(*started);
}

CompletionProviderResult
EditorApplication::dispatch_completion_provider(CompletionProvider provider,
                                                const CompletionRequest& request) {
    const Buffer* buffer = runtime_.buffers().try_get(request.target.buffer);
    if (buffer == nullptr || buffer->snapshot().revision() != request.revision) {
        throw std::runtime_error("completion provider received a stale request");
    }
    const DocumentSnapshot snapshot = buffer->snapshot();
    if (provider.kind == CompletionProviderKind::Scripted) {
        std::expected<CompletionProviderResult, std::string> response =
            guile_.complete(provider, request);
        if (!response) {
            throw std::runtime_error(response.error());
        }
        return std::move(*response);
    }
    if (provider.kind == CompletionProviderKind::Lsp) {
        LspSession* session = lsp_sessions_->find(LspSessionId{provider.session});
        if (session == nullptr) {
            throw std::runtime_error("completion references an unknown LSP session");
        }
        if (!buffer->resource_uri()) {
            throw std::runtime_error("LSP completion requires a file-backed buffer");
        }
        LspCompletionTriggerKind trigger = LspCompletionTriggerKind::Invoked;
        if (request.trigger.kind == CompletionTriggerKind::Character) {
            trigger = LspCompletionTriggerKind::TriggerCharacter;
        } else if (request.trigger.kind == CompletionTriggerKind::Incomplete) {
            trigger = LspCompletionTriggerKind::TriggerForIncompleteCompletions;
        }
        LspCompletionRequest lsp_request{.uri = path_to_file_uri(*buffer->resource_uri()),
                                         .language_id = session->config().language_id,
                                         .revision = request.revision,
                                         .text = snapshot.content(),
                                         .caret = request.caret,
                                         .trigger = trigger,
                                         .trigger_character = request.trigger.character};
        auto bridge = std::make_shared<LspCompletionBridge>(
            LspCompletionBridge{provider, request, std::move(lsp_request)});
        return CompletionProviderAsync{
            .start = [session, bridge = std::move(bridge)](
                         CompletionProviderAsync::Completed completed,
                         CompletionProviderAsync::Failed failed,
                         CompletionProviderAsync::Cancelled cancelled) mutable
                -> std::expected<CompletionProviderAsync::Cancel, std::string> {
                try {
                    const Text response_text = bridge->lsp_request.text;
                    CompletionProviderAsync::Failed conversion_failed = failed;
                    return LspCompletionFeature::request(
                        *session, std::move(bridge->lsp_request),
                        [bridge, response_text, completed = std::move(completed),
                         failed = std::move(conversion_failed)](
                            LspCompletionResponse response) mutable noexcept {
                            try {
                                completed(completion_response_from_lsp(
                                    bridge->provider, bridge->request, response_text,
                                    std::move(response)));
                            } catch (const std::exception& exception) {
                                try {
                                    failed(exception.what());
                                } catch (...) {
                                    return;
                                }
                            } catch (...) {
                                try {
                                    failed("unknown LSP completion conversion failure");
                                } catch (...) {
                                    return;
                                }
                            }
                        },
                        std::move(failed), std::move(cancelled));
                } catch (const std::exception& exception) {
                    return std::unexpected(exception.what());
                } catch (...) {
                    return std::unexpected("unknown LSP completion startup failure");
                }
            }};
    }
    return CompletionProviderResponse{.provider = provider, .items = {}, .is_incomplete = false};
}

std::expected<CompletionProviderAsync::Cancel, std::string>
EditorApplication::dispatch_completion_resolve(const CompletionRequest& request,
                                               const CompletionItem& item,
                                               CompletionPipeline::ResolveCompleted completed,
                                               CompletionProviderAsync::Failed failed,
                                               CompletionProviderAsync::Cancelled cancelled) {
    if (item.provider.kind == CompletionProviderKind::Scripted) {
        std::expected<CompletionItem, std::string> resolved =
            guile_.resolve(item.provider, request, item);
        if (!resolved) {
            return std::unexpected(std::move(resolved.error()));
        }
        completed(std::move(*resolved));
        return CompletionProviderAsync::Cancel{};
    }
    if (item.provider.kind != CompletionProviderKind::Lsp) {
        return std::unexpected("completion item provider does not support resolution");
    }
    LspSession* session = lsp_sessions_->find(LspSessionId{item.provider.session});
    if (session == nullptr) {
        return std::unexpected("completion resolve references an unknown LSP session");
    }
    const Buffer* buffer = runtime_.buffers().try_get(request.target.buffer);
    if (buffer == nullptr || buffer->snapshot().revision() != request.revision) {
        return std::unexpected("completion resolve targets a stale buffer");
    }
    if (item.raw.empty()) {
        return std::unexpected("LSP completion item has no resolve payload");
    }
    auto response_text = std::make_shared<Text>(buffer->snapshot().content());
    auto response_request = std::make_shared<CompletionRequest>(request);
    const CompletionProvider provider = item.provider;
    CompletionProviderAsync::Failed conversion_failed = failed;
    return LspCompletionFeature::resolve(
        *session, item.raw,
        [provider, request = std::move(response_request), text = std::move(response_text),
         completed = std::move(completed),
         failed = std::move(conversion_failed)](LspCompletionItem resolved) mutable noexcept {
            try {
                LspCompletionResponse response{.items = {std::move(resolved)},
                                               .is_incomplete = false};
                CompletionProviderResponse converted =
                    completion_response_from_lsp(provider, *request, *text, std::move(response));
                if (converted.items.size() != 1) {
                    report_completion_failure(failed, "LSP completion resolve returned no item");
                    return;
                }
                completed(std::move(converted.items.front()));
            } catch (const std::exception& exception) {
                report_completion_failure(failed, exception.what());
            } catch (...) {
                report_completion_failure(failed,
                                          "unknown LSP completion resolve conversion failure");
            }
        },
        std::move(failed), std::move(cancelled));
}

void EditorApplication::after_edit(ViewId view) {
    const BufferId buffer = runtime_.views().get(view).buffer_id();
    const RevisionId revision = runtime_.buffers().get(buffer).snapshot().revision();
    if (const std::expected<void, std::string> notified =
            guile_.buffer_edited(buffer, view, revision);
        !notified) {
        set_message(notified.error());
    }
}

} // namespace cind
