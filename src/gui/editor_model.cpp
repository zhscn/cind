#include "gui/editor_model.hpp"

#include "document/text.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>

namespace cind::gui {

namespace {

std::vector<ChromeItem> completion_items(const EditorApplication& application) {
    std::vector<ChromeItem> items;
    const CompletionState* completion = application.completion().state();
    if (completion == nullptr) {
        return items;
    }
    items.reserve(completion->matches.size());
    for (const CompletionMatch& match : completion->matches) {
        items.push_back(
            {.label = match.item.label, .detail = match.item.detail, .kind = match.item.kind});
    }
    return items;
}

std::optional<std::string_view> completion_documentation(const CompletionState* completion) {
    if (completion == nullptr || completion->selected >= completion->matches.size()) {
        return std::nullopt;
    }
    const std::string& documentation = completion->matches[completion->selected].item.documentation;
    return documentation.empty() ? std::nullopt : std::optional<std::string_view>(documentation);
}

} // namespace

EditorModel::EditorModel(std::string path, std::optional<std::string> initial,
                         std::uint32_t initial_line, EditorPlatformServices platform_services,
                         std::optional<std::string> init_file)
    : application_({.path = std::move(path),
                    .initial_text = std::move(initial),
                    .initial_line = initial_line,
                    .platform_services = std::move(platform_services),
                    .init_file = std::move(init_file)}) {}

void EditorModel::layout_view(int rows, int columns, float visible_text_rows) {
    const ChromeContent chrome = application_.chrome_content(preedit_);
    const std::vector<ChromeItem> completions = completion_items(application_);
    const CompletionState* completion = application_.completion().state();
    const std::optional<std::size_t> completion_selection =
        completion != nullptr && !completion->matches.empty() ? std::optional(completion->selected)
                                                              : std::nullopt;
    application_.resolve_completion_window(completion_viewport_.first_item(), 8);

    if (application_.window_layout().leaves().size() > 1) {
        const WindowPartition partition = application_.window_layout().partition(rows - 1, columns);
        for (const WindowPlacement& placement : partition.windows) {
            EditSession& pane_session = application_.session(placement.window);
            const DocumentSnapshot pane_snapshot = pane_session.snapshot();
            ViewportState& pane_state = pane_session.view().viewport();
            ui::EditorSceneViewState pane_view{
                .viewport = {.top_line = pane_state.top_line,
                             .top_line_offset = pane_state.top_line_offset,
                             .left_column = pane_state.left_column},
                .popup = {},
                .completion = placement.window == application_.window_id() ? completion_viewport_
                                                                           : ui::ListViewport{},
            };
            pane_view = ui::layout_editor_scene(
                {.text = pane_snapshot.content(),
                 .caret = pane_session.caret(),
                 .rows = std::max(3, placement.rect.rows + 1),
                 .cols = std::max(1, placement.rect.columns),
                 .visible_text_rows = static_cast<float>(std::max(1, placement.rect.rows - 1)),
                 .tab_width = pane_session.style().tab_width,
                 .reveal_caret =
                     placement.window == application_.window_id() && application_.reveal_caret(),
                 .popup_item_count = 0,
                 .popup_capacity = 0,
                 .popup_selection = std::nullopt,
                 .completion_item_count =
                     placement.window == application_.window_id() ? completions.size() : 0,
                 .completion_selection = placement.window == application_.window_id()
                                             ? completion_selection
                                             : std::nullopt},
                pane_view);
            pane_state.top_line = pane_view.viewport.top_line;
            pane_state.top_line_offset = pane_view.viewport.top_line_offset;
            pane_state.left_column = pane_view.viewport.left_column;
            if (placement.window == application_.window_id()) {
                completion_viewport_ = pane_view.completion;
            }
        }

        EditSession& active = application_.session();
        const DocumentSnapshot active_snapshot = active.snapshot();
        const ViewportState& active_state = active.view().viewport();
        ui::EditorSceneViewState popup_view{
            .viewport = {.top_line = active_state.top_line,
                         .top_line_offset = active_state.top_line_offset,
                         .left_column = active_state.left_column},
            .popup = popup_viewport_,
            .completion = completion_viewport_,
        };
        popup_view = ui::layout_editor_scene({.text = active_snapshot.content(),
                                              .caret = active.caret(),
                                              .rows = rows,
                                              .cols = columns,
                                              .visible_text_rows = visible_text_rows,
                                              .tab_width = active.style().tab_width,
                                              .reveal_caret = false,
                                              .popup_item_count = chrome.popup_items.size(),
                                              .popup_capacity = chrome.popup_capacity,
                                              .popup_selection = chrome.popup_selection,
                                              .completion_item_count = completions.size(),
                                              .completion_selection = completion_selection},
                                             popup_view);
        popup_viewport_ = popup_view.popup;
        completion_viewport_ = popup_view.completion;
        last_rows_ = rows;
        return;
    }

    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();

    ViewportState& state = session.view().viewport();
    ui::EditorSceneViewState view{
        .viewport = {.top_line = state.top_line,
                     .top_line_offset = state.top_line_offset,
                     .left_column = state.left_column},
        .popup = popup_viewport_,
        .completion = completion_viewport_,
    };
    view = ui::layout_editor_scene({.text = snapshot.content(),
                                    .caret = session.caret(),
                                    .rows = rows,
                                    .cols = columns,
                                    .visible_text_rows = visible_text_rows,
                                    .tab_width = session.style().tab_width,
                                    .reveal_caret = application_.reveal_caret(),
                                    .popup_item_count = chrome.popup_items.size(),
                                    .popup_capacity = chrome.popup_capacity,
                                    .popup_selection = chrome.popup_selection,
                                    .completion_item_count = completions.size(),
                                    .completion_selection = completion_selection},
                                   view);
    state.top_line = view.viewport.top_line;
    state.top_line_offset = view.viewport.top_line_offset;
    state.left_column = view.viewport.left_column;
    popup_viewport_ = view.popup;
    completion_viewport_ = view.completion;
    last_rows_ = rows;
}

ui::Scene EditorModel::compose(int rows, int columns, float visible_text_rows) {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const ChromeContent chrome = application_.chrome_content(preedit_);
    application_.resolve_completion_window(completion_viewport_.first_item(), 8);
    const std::vector<ChromeItem> completions = completion_items(application_);
    const CompletionState* completion = application_.completion().state();
    const std::optional<std::size_t> completion_selection =
        completion != nullptr && !completion->matches.empty() ? std::optional(completion->selected)
                                                              : std::nullopt;
    const std::optional<TextOffset> completion_anchor =
        completion != nullptr ? std::optional(completion->request.anchor) : std::nullopt;
    const std::optional<std::string_view> completion_docs = completion_documentation(completion);
    const std::optional<std::string_view> popup_input =
        chrome.popup_input ? std::optional<std::string_view>(*chrome.popup_input) : std::nullopt;
    const InputStateRegistry::Definition& active_input_state = application_.input_state();
    const ViewportState& state = session.view().viewport();
    const ui::EditorSceneViewState view{
        .viewport = {.top_line = state.top_line,
                     .top_line_offset = state.top_line_offset,
                     .left_column = state.left_column},
        .popup = popup_viewport_,
        .completion = completion_viewport_,
    };
    const std::vector<TextRange> active_selections = session.selected_ranges();
    PositionHintProviderResult active_hint_result =
        application_.position_hints(application_.window_id());
    const std::vector<PositionHint> active_position_hints =
        active_hint_result ? std::move(*active_hint_result) : std::vector<PositionHint>{};
    const ModelineContent active_modeline = application_.modeline(application_.window_id());
    const ui::EditorSceneInput active_input{.text = snapshot.content(),
                                            .tokens = application_.syntax_tokens(),
                                            .signs = signs(application_.window_id()),
                                            .diagnostic_signs =
                                                &diagnostic_signs(application_.window_id()),
                                            .caret = session.caret(),
                                            .selections = active_selections,
                                            .position_hints = active_position_hints,
                                            .rows = rows,
                                            .cols = columns,
                                            .visible_text_rows = visible_text_rows,
                                            .tab_width = session.style().tab_width,
                                            .revision = snapshot.revision(),
                                            .modeline = active_modeline,
                                            .cursor_shape = active_input_state.cursor,
                                            .pending_key = chrome.pending_key,
                                            .echo = chrome.echo,
                                            .echo_cursor_column = chrome.echo_cursor_column,
                                            .echo_cursor_byte = chrome.echo_cursor_byte,
                                            .popup_title = chrome.popup_title,
                                            .popup_items = chrome.popup_items,
                                            .popup_capacity = chrome.popup_capacity,
                                            .popup_selection = chrome.popup_selection,
                                            .popup_input = popup_input,
                                            .popup_input_cursor = chrome.popup_input_cursor,
                                            .completion_items = completions,
                                            .completion_selection = completion_selection,
                                            .completion_anchor = completion_anchor,
                                            .completion_documentation = completion_docs};
    if (application_.window_layout().leaves().size() == 1) {
        return ui::compose_editor_scene(active_input, view);
    }

    const WindowPartition partition = application_.window_layout().partition(rows - 1, columns);
    std::vector<ui::EditorPaneScene> panes;
    panes.reserve(partition.windows.size());
    for (const WindowPlacement& placement : partition.windows) {
        EditSession& pane_session = application_.session(placement.window);
        const DocumentSnapshot pane_snapshot = pane_session.snapshot();
        const ViewportState& pane_state = pane_session.view().viewport();
        const bool active = placement.window == application_.window_id();
        const ui::EditorSceneViewState pane_view{
            .viewport = {.top_line = pane_state.top_line,
                         .top_line_offset = pane_state.top_line_offset,
                         .left_column = pane_state.left_column},
            .popup = {},
            .completion = active ? completion_viewport_ : ui::ListViewport{},
        };
        const InputStateRegistry::Definition& pane_input_state =
            application_.input_state(placement.window);
        const std::vector<TextRange> pane_selections = pane_session.selected_ranges();
        PositionHintProviderResult pane_hint_result = application_.position_hints(placement.window);
        const std::vector<PositionHint> pane_position_hints =
            pane_hint_result ? std::move(*pane_hint_result) : std::vector<PositionHint>{};
        const ModelineContent pane_modeline = application_.modeline(placement.window);
        ui::Scene pane_scene = ui::compose_editor_scene(
            {.text = pane_snapshot.content(),
             .tokens = application_.syntax_tokens(placement.window),
             .signs = signs(placement.window),
             .diagnostic_signs = &diagnostic_signs(placement.window),
             .caret = pane_session.caret(),
             .selections = pane_selections,
             .position_hints = pane_position_hints,
             .rows = std::max(3, placement.rect.rows + 1),
             .cols = std::max(1, placement.rect.columns),
             .visible_text_rows = static_cast<float>(std::max(1, placement.rect.rows - 1)),
             .tab_width = pane_session.style().tab_width,
             .revision = pane_snapshot.revision(),
             .modeline = pane_modeline,
             .cursor_shape = pane_input_state.cursor,
             .pending_key = {},
             .echo = {},
             .echo_cursor_column = std::nullopt,
             .echo_cursor_byte = std::nullopt,
             .popup_title = {},
             .popup_items = {},
             .popup_capacity = 0,
             .popup_selection = std::nullopt,
             .popup_input = std::nullopt,
             .popup_input_cursor = std::nullopt,
             .completion_items =
                 active ? std::span<const ChromeItem>(completions) : std::span<const ChromeItem>{},
             .completion_selection = active ? completion_selection : std::nullopt,
             .completion_anchor = active ? completion_anchor : std::nullopt,
             .completion_documentation = active ? completion_docs : std::nullopt},
            pane_view);
        panes.push_back({.id = pane_id(placement.window),
                         .rect = {.row = placement.rect.row,
                                  .col = placement.rect.column,
                                  .rows = placement.rect.rows,
                                  .cols = placement.rect.columns},
                         .active = active,
                         .scene = std::move(pane_scene)});
    }
    std::vector<ui::SceneDivider> dividers;
    dividers.reserve(partition.dividers.size());
    for (std::size_t index = 0; index < partition.dividers.size(); ++index) {
        const WindowDivider& divider = partition.dividers[index];
        dividers.push_back({.id = std::format("workspace/divider/{}", index),
                            .axis = divider.axis == WindowSplitAxis::Rows
                                        ? ui::DividerAxis::Horizontal
                                        : ui::DividerAxis::Vertical,
                            .position = divider.position,
                            .start = divider.start,
                            .length = divider.length});
    }
    return ui::compose_editor_workspace({.rows = rows, .cols = columns}, std::move(panes),
                                        std::move(dividers),
                                        ui::compose_editor_scene(active_input, view));
}

bool EditorModel::handle_key(KeyStroke key, int page_rows) {
    return application_.handle_key(key, page_rows);
}

void EditorModel::insert_text(std::string_view text) {
    application_.insert_text(text);
    preedit_.clear();
}

void EditorModel::set_preedit(std::string_view text) {
    preedit_ = text.empty() || application_.text_input_policy() == TextInputPolicy::Ignore
                   ? std::string()
                   : std::format("IME · {}", text);
}

void EditorModel::click(const ui::HitTarget& target) {
    const PointerTargetKind target_kind = [&] {
        switch (target.kind) {
        case ui::HitTargetKind::DocumentText:
            return PointerTargetKind::DocumentText;
        case ui::HitTargetKind::DocumentGutter:
            return PointerTargetKind::DocumentGutter;
        case ui::HitTargetKind::PopupHeader:
            return PointerTargetKind::PopupHeader;
        case ui::HitTargetKind::PopupItem:
            return PointerTargetKind::PopupItem;
        case ui::HitTargetKind::Status:
            return PointerTargetKind::Status;
        case ui::HitTargetKind::Echo:
            return PointerTargetKind::Echo;
        case ui::HitTargetKind::Region:
            return PointerTargetKind::Region;
        }
        throw std::logic_error("unknown UI pointer target kind");
    }();
    PointerEvent event{.target = target_kind,
                       .window = std::nullopt,
                       .document_line = target.document_line,
                       .display_column = target.display_column
                                             ? std::optional(static_cast<std::uint32_t>(
                                                   std::max(0, *target.display_column)))
                                             : std::nullopt,
                       .popup_item = target.popup_item};
    if (!target.pane_id.empty()) {
        for (const OpenWindowSnapshot& window : application_.open_windows()) {
            if (pane_id(window.window) == target.pane_id) {
                event.window = window.window;
                break;
            }
        }
        if (!event.window) {
            return;
        }
    }
    (void)application_.handle_pointer(event);
}

void EditorModel::scroll(ScrollInput input) {
    (void)application_.handle_scroll(input);
}

EditorRenderState EditorModel::render_state() {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const ViewportState& viewport = session.view().viewport();
    const WindowId window = application_.window_id();
    const ViewId view = application_.view_id();
    const BufferId buffer = application_.buffer_id(window);
    return {.revision = snapshot.revision(),
            .viewport = {.top_line = viewport.top_line,
                         .top_line_offset = viewport.top_line_offset,
                         .left_column = viewport.left_column},
            .reveal_caret = application_.reveal_caret(),
            .window_slot = window.slot,
            .window_generation = window.generation,
            .view_slot = view.slot,
            .view_generation = view.generation,
            .buffer_slot = buffer.slot,
            .buffer_generation = buffer.generation};
}

EditorStateSnapshot EditorModel::inspect() {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = session.caret();
    const ViewportState& view = session.view().viewport();
    const CommandLoop& command_loop = application_.command_loop();
    const EditorRuntime& runtime = application_.runtime();
    CommandLoopStateSnapshot command_state{
        .keymaps = {},
        .layers = {},
        .override_keymaps = {},
        .pending_keys = application_.pending_key_sequence_text(),
        .pending_keymap = {},
        .pending_input_state = application_.pending_input_state_name(),
        .repeat_count = command_loop.repeat_count(),
        .register_name = command_loop.pending_prefix().register_name,
        .prefix_extra = command_loop.pending_prefix().extra,
        .prefix_text = application_.pending_prefix_text(),
        .last_command = application_.last_command()};
    for (const KeymapLayer& layer : command_loop.keymap_layers()) {
        command_state.keymaps.push_back(runtime.keymaps().definition(layer.keymap).name);
        KeymapLayerStateSnapshot layer_state{.name =
                                                 runtime.keymaps().definition(layer.keymap).name,
                                             .scope = layer.scope,
                                             .parents = {}};
        for (std::optional<KeymapId> parent = runtime.keymaps().parent(layer.keymap); parent;
             parent = runtime.keymaps().parent(*parent)) {
            layer_state.parents.push_back(runtime.keymaps().definition(*parent).name);
        }
        command_state.layers.push_back(std::move(layer_state));
    }
    for (const KeymapId keymap : command_loop.override_keymaps()) {
        command_state.override_keymaps.push_back(runtime.keymaps().definition(keymap).name);
    }
    if (const std::optional<KeymapId> keymap = command_loop.pending_keymap()) {
        command_state.pending_keymap = runtime.keymaps().definition(*keymap).name;
    }
    GuileRuntimeSnapshot guile = application_.scripting();
    ScriptingStateSnapshot scripting_state{
        .engine = std::move(guile.engine),
        .version = std::move(guile.version),
        .modules = std::move(guile.modules),
        .extensions = std::move(guile.extensions),
        .command_revision = guile.command_revision,
        .scripted_commands = guile.scripted_commands,
        .provider_revision = guile.provider_revision,
        .scripted_providers = guile.scripted_providers,
        .binding_revision = guile.binding_revision,
        .input_state_revision = guile.input_state_revision,
        .scripted_input_states = guile.scripted_input_states,
        .scripted_input_strategies = guile.scripted_input_strategies,
        .mode_revision = guile.mode_revision,
        .scripted_modes = guile.scripted_modes,
        .resource_policy_revision = guile.resource_policy_revision,
        .scripted_file_mode_rules = guile.scripted_file_mode_rules,
        .scripted_project_providers = guile.scripted_project_providers,
        .outstanding_async_tasks = guile.outstanding_async_tasks,
        .last_error = std::move(guile.last_error)};
    InteractionStateSnapshot interaction_state;
    if (const InteractionState* interaction = application_.interaction().state()) {
        const std::string input = application_.interaction().input_text();
        interaction_state = {
            .active = true,
            .window_slot = interaction->window.slot,
            .window_generation = interaction->window.generation,
            .buffer_slot = interaction->buffer.slot,
            .buffer_generation = interaction->buffer.generation,
            .view_slot = interaction->view.slot,
            .view_generation = interaction->view.generation,
            .origin_window_slot = interaction->origin.window.slot,
            .origin_window_generation = interaction->origin.window.generation,
            .origin_buffer_slot = interaction->origin.buffer.slot,
            .origin_buffer_generation = interaction->origin.buffer.generation,
            .origin_view_slot = interaction->origin.view.slot,
            .origin_view_generation = interaction->origin.view.generation,
            .kind = interaction->request.kind == InteractionKind::Picker ? "picker" : "text",
            .keymap = interaction->request.keymap,
            .input_state = interaction->request.input_state,
            .buffer_name = interaction->request.buffer_name,
            .prompt = interaction->request.prompt,
            .input = input,
            .input_cursor = application_.interaction().input_caret().value,
            .history = interaction->request.history,
            .history_entries =
                application_.interaction().history(interaction->request.history).size(),
            .history_index = interaction->history_index,
            .history_draft = interaction->history_draft,
            .provider = interaction->request.provider,
            .allow_custom_input = interaction->request.allow_custom_input,
            .generation = interaction->generation,
            .loading = interaction->loading,
            .selected = interaction->selected,
            .error = interaction->error,
            .candidates = {}};
        interaction_state.candidates.reserve(interaction->candidates.size());
        for (const InteractionCandidate& candidate : interaction->candidates) {
            interaction_state.candidates.push_back(
                {.value = candidate.value, .label = candidate.label, .detail = candidate.detail});
        }
    }
    CompletionStateSnapshot completion_state;
    if (const CompletionState* completion = application_.completion().state()) {
        completion_state.active = true;
        completion_state.generation = completion->request.generation;
        completion_state.revision = completion->request.revision;
        completion_state.anchor = completion->request.anchor;
        completion_state.caret = completion->request.caret;
        completion_state.query = completion->request.query;
        completion_state.selected = completion->selected;
        for (const CompletionProviderState& provider : completion->providers) {
            if (provider.pending) {
                completion_state.pending_providers.push_back(
                    completion_provider_name(provider.provider));
            }
        }
        completion_state.items.reserve(completion->matches.size());
        for (const CompletionMatch& match : completion->matches) {
            completion_state.items.push_back(
                {.id = match.item.id,
                 .provider = completion_provider_name(match.item.provider),
                 .label = match.item.label,
                 .kind = match.item.kind,
                 .detail = match.item.detail,
                 .resolved = match.item.resolved,
                 .resolving = match.item.resolving,
                 .resolve_error = match.item.resolve_error,
                 .documentation = match.item.documentation});
        }
    }
    std::vector<LspSessionStateSnapshot> lsp_state;
    for (const LspSessionSnapshot& lsp_session : application_.lsp_sessions()) {
        std::string state;
        switch (lsp_session.state) {
        case LspSessionState::Stopped:
            state = "stopped";
            break;
        case LspSessionState::Starting:
            state = "starting";
            break;
        case LspSessionState::Initializing:
            state = "initializing";
            break;
        case LspSessionState::Ready:
            state = "ready";
            break;
        case LspSessionState::Failed:
            state = "failed";
            break;
        }
        lsp_state.push_back({.id = lsp_session.id.value,
                             .state = std::move(state),
                             .command = lsp_session.command,
                             .root = lsp_session.root,
                             .pending_requests = lsp_session.pending_requests,
                             .open_documents = lsp_session.open_documents,
                             .server_capabilities = lsp_session.server_capabilities,
                             .error = lsp_session.error});
    }
    std::vector<OpenBufferStateSnapshot> buffers;
    for (const OpenBufferSnapshot& buffer : application_.open_buffers()) {
        const ViewId buffer_view = buffer.view.value_or(ViewId{});
        std::vector<ModeThingStateSnapshot> things;
        things.reserve(buffer.things.size());
        for (const ModeThingBinding& thing : buffer.things) {
            things.push_back({.name = thing.name, .definition = thing.definition});
        }
        buffers.push_back({.buffer_slot = buffer.buffer.slot,
                           .buffer_generation = buffer.buffer.generation,
                           .view_present = buffer.view.has_value(),
                           .view_slot = buffer_view.slot,
                           .view_generation = buffer_view.generation,
                           .name = buffer.name,
                           .resource = buffer.resource.value_or(std::string()),
                           .modified = buffer.modified,
                           .active = buffer.active,
                           .saving = buffer.saving,
                           .major_mode = buffer.major_mode,
                           .interaction_class = buffer.interaction_class,
                           .initial_input_state = buffer.initial_input_state,
                           .completion_auto = buffer.completion_auto,
                           .things = std::move(things),
                           .completion_providers = buffer.completion_providers,
                           .location_count = buffer.location_count,
                           .diagnostic_count = buffer.diagnostic_count,
                           .diagnostic_errors = buffer.diagnostic_errors,
                           .diagnostic_warnings = buffer.diagnostic_warnings});
    }
    const auto inspect_window = [&](WindowId window, bool active) {
        const Window& definition = runtime.windows().get(window);
        const ViewId view = definition.view_id();
        const BufferId buffer = runtime.views().get(view).buffer_id();
        std::vector<std::string> input_states;
        for (const InputStateId state : runtime.views().get(view).input_states().stack()) {
            input_states.push_back(runtime.input_states().definition(state).name);
        }
        return OpenWindowStateSnapshot{.window_slot = window.slot,
                                       .window_generation = window.generation,
                                       .view_slot = view.slot,
                                       .view_generation = view.generation,
                                       .buffer_slot = buffer.slot,
                                       .buffer_generation = buffer.generation,
                                       .role = definition.role().value_or(std::string{}),
                                       .pinned = definition.pinned(),
                                       .created_by_policy = definition.created_by_policy(),
                                       .active = active,
                                       .input_states = std::move(input_states)};
    };
    std::vector<OpenWindowStateSnapshot> windows;
    for (const OpenWindowSnapshot& window : application_.open_windows()) {
        windows.push_back(inspect_window(window.window, window.active));
    }
    const auto inspect_layout =
        [](this const auto& self,
           const WorkbenchLayoutSnapshot& node) -> WorkbenchLayoutStateSnapshot {
        if (node.window) {
            return {.leaf = true,
                    .window = {.slot = node.window->slot, .generation = node.window->generation},
                    .axis = {},
                    .ratio = 0.5F,
                    .children = {}};
        }
        std::vector<WorkbenchLayoutStateSnapshot> children;
        children.reserve(node.children.size());
        for (const WorkbenchLayoutSnapshot& child : node.children) {
            children.push_back(self(child));
        }
        return {.leaf = false,
                .window = {},
                .axis = node.axis == WindowSplitAxis::Rows ? "rows" : "columns",
                .ratio = node.ratio,
                .children = std::move(children)};
    };
    std::vector<WorkbenchStateSnapshot> workbenches;
    for (const WorkbenchSnapshot& workbench : application_.workbench_snapshots()) {
        WorkbenchStateSnapshot inspected{
            .workbench = {.slot = workbench.workbench.slot,
                          .generation = workbench.workbench.generation},
            .name = workbench.name,
            .active = workbench.active,
            .scope = {},
            .mru = {},
            .active_window = {.slot = workbench.active_window.slot,
                              .generation = workbench.active_window.generation},
            .slots = {},
            .layout = inspect_layout(workbench.layout),
            .windows = {}};
        inspected.scope.reserve(workbench.scope.size());
        for (const ProjectId project : workbench.scope) {
            inspected.scope.push_back({.slot = project.slot, .generation = project.generation});
        }
        inspected.mru.reserve(workbench.mru.size());
        for (const BufferId buffer : workbench.mru) {
            inspected.mru.push_back({.slot = buffer.slot, .generation = buffer.generation});
        }
        inspected.slots.reserve(workbench.slots.size());
        for (const auto& [role, window] : workbench.slots) {
            inspected.slots.push_back(
                {.role = role, .window = {.slot = window.slot, .generation = window.generation}});
        }
        inspected.windows.reserve(workbench.windows.size());
        for (const WindowId window : workbench.windows) {
            inspected.windows.push_back(inspect_window(window, window == workbench.active_window));
        }
        workbenches.push_back(std::move(inspected));
    }
    std::vector<ProjectStateSnapshot> projects;
    for (const ProjectId project_id : runtime.projects().all()) {
        const Project& project = runtime.projects().get(project_id);
        projects.push_back({.project_slot = project_id.slot,
                            .project_generation = project_id.generation,
                            .name = project.name(),
                            .roots = project.roots(),
                            .discovery_provider = project.discovery_provider(),
                            .discovery_marker = project.discovery_marker(),
                            .file_count = project.files().size(),
                            .index_revision = project.index_revision(),
                            .indexing = project.indexing(),
                            .index_error = project.index_error().value_or(std::string())});
    }
    LocationStateSnapshot location_at_caret;
    if (const BufferLocation* location = session.buffer().location_at(caret)) {
        location_at_caret = {.present = true,
                             .source_range = location->source_range,
                             .resource = location->resource,
                             .target = location->target};
    }
    LocationNavigationStateSnapshot location_navigation;
    const LocationNavigationSnapshot navigation = application_.location_navigation();
    if (navigation.buffer) {
        location_navigation = {.present = true,
                               .buffer_slot = navigation.buffer->slot,
                               .buffer_generation = navigation.buffer->generation,
                               .selected_index = navigation.selected_index,
                               .location_count = navigation.location_count};
    }
    std::vector<WorkbenchJumpStateSnapshot> jumps;
    for (const WorkbenchJumpSnapshot& graph : application_.jump_graphs()) {
        WorkbenchJumpStateSnapshot inspected{
            .workbench = {.slot = graph.workbench.slot, .generation = graph.workbench.generation},
            .nodes = {},
            .edges = {},
            .walks = {}};
        inspected.nodes.reserve(graph.nodes.size());
        for (const JumpNode& node : graph.nodes) {
            inspected.nodes.push_back({.id = node.id,
                                       .attached = node.position.buffer.valid(),
                                       .buffer = {.slot = node.position.buffer.slot,
                                                  .generation = node.position.buffer.generation},
                                       .anchor = node.position.anchor,
                                       .resource = node.position.resource,
                                       .fallback = node.position.fallback,
                                       .excerpt = node.position.excerpt,
                                       .created_at = node.created_at,
                                       .last_visit = node.last_visit});
        }
        inspected.edges.reserve(graph.edges.size());
        for (const JumpEdge& edge : graph.edges) {
            inspected.edges.push_back({.from = edge.from,
                                       .to = edge.to,
                                       .kind = edge.kind,
                                       .at = edge.at,
                                       .persistent = edge.persistent});
        }
        inspected.walks.reserve(graph.walks.size());
        for (const JumpWalkSnapshot& walk : graph.walks) {
            inspected.walks.push_back(
                {.window = {.slot = walk.window.slot, .generation = walk.window.generation},
                 .entries = walk.entries,
                 .cursor = walk.cursor});
        }
        jumps.push_back(std::move(inspected));
    }
    const WindowId active_window = application_.window_id();
    const InputStateRegistry::Definition& input_state = application_.input_state();
    const View& active_view = application_.runtime().views().get(application_.view_id());
    const View& focused_input_view =
        application_.interaction().active()
            ? application_.runtime().views().get(application_.interaction().state()->view)
            : active_view;
    const ViewSelection view_selection = session.selection_model();
    SelectionStateSnapshot selection_state{
        .active = session.mark().has_value(),
        .primary = view_selection.primary,
        .history_depth =
            application_.runtime().views().selection_history_size(application_.view_id()),
        .metadata = view_selection.metadata,
        .ranges = {}};
    selection_state.ranges.reserve(view_selection.ranges.size());
    for (const SelectionRange& range : view_selection.ranges) {
        selection_state.ranges.push_back(
            {.anchor = range.anchor,
             .head = range.head,
             .granularity = std::string(selection_granularity_name(range.granularity))});
    }
    const std::optional<InputStrategyId> input_strategy =
        focused_input_view.input_strategy()
            ? focused_input_view.input_strategy()
            : application_.runtime().input_strategies().default_strategy();
    PositionHintsStateSnapshot position_hints{
        .provider = static_cast<bool>(input_state.position_hints), .items = {}, .error = {}};
    PositionHintProviderResult position_hint_result =
        application_.interaction().active()
            ? PositionHintProviderResult{std::vector<PositionHint>{}}
            : application_.position_hints(application_.window_id());
    if (position_hint_result) {
        position_hints.items.reserve(position_hint_result->size());
        for (const PositionHint& hint : *position_hint_result) {
            position_hints.items.push_back({.position = hint.position, .label = hint.label});
        }
    } else {
        position_hints.error = std::move(position_hint_result.error());
    }
    return {.path = application_.path(),
            .revision = snapshot.revision(),
            .document_bytes = text.size_bytes(),
            .line_count = text.line_count(),
            .dirty = application_.dirty(),
            .caret = caret,
            .caret_position = text.position(caret),
            .caret_display_column = ui::display_column(text, caret, session.style().tab_width),
            .selection = std::move(selection_state),
            .viewport = {.top_line = view.top_line,
                         .top_line_offset = view.top_line_offset,
                         .left_column = view.left_column},
            .line_signs = signs(application_.window_id()),
            .tab_width = session.style().tab_width,
            .style_origin = application_.style_origin(),
            .message = application_.message(),
            .preedit = preedit_,
            .last_key = application_.last_key(),
            .active_window_slot = active_window.slot,
            .active_window_generation = active_window.generation,
            .input_focus = std::string(application_.input_focus()),
            .input_strategy =
                input_strategy
                    ? application_.runtime().input_strategies().definition(*input_strategy).name
                    : std::string(),
            .input_state = input_state.name,
            .input_cursor_shape = std::string(cursor_shape_name(input_state.cursor)),
            .input_state_indicator = input_state.indicator,
            .text_input_policy =
                application_.text_input_policy() == TextInputPolicy::Accept ? "accept" : "ignore",
            .text_input_command = input_state.text_command.value_or(std::string()),
            .text_input_command_available =
                input_state.text_command &&
                application_.runtime().commands().find(*input_state.text_command).has_value(),
            .selection_after_edit = std::string(selection_edit_policy_name(
                application_.runtime().selection_edit_policy(focused_input_view.id()))),
            .input_state_handler = static_cast<bool>(input_state.handler),
            .input_state_on_enter = static_cast<bool>(input_state.on_enter),
            .input_state_on_exit = static_cast<bool>(input_state.on_exit),
            .position_hints = std::move(position_hints),
            .command_loop = std::move(command_state),
            .scripting = std::move(scripting_state),
            .interaction = std::move(interaction_state),
            .completion = std::move(completion_state),
            .lsp = std::move(lsp_state),
            .buffers = std::move(buffers),
            .windows = std::move(windows),
            .workbenches = std::move(workbenches),
            .projects = std::move(projects),
            .location_at_caret = std::move(location_at_caret),
            .location_navigation = location_navigation,
            .jumps = std::move(jumps),
            .background_work = application_.has_background_work(),
            .project_search_running = application_.project_search_running(),
            .quit = application_.should_quit()};
}

const ui::LineSigns& EditorModel::signs(WindowId window) {
    const BufferId buffer = application_.buffer_id(window);
    const DocumentSnapshot snapshot = application_.session(window).snapshot();
    auto found = std::ranges::find_if(
        sign_caches_, [buffer](const SignCache& cache) { return cache.buffer == buffer; });
    if (found == sign_caches_.end()) {
        sign_caches_.push_back({.buffer = buffer, .signs = {}, .diagnostic_signs = {}});
        found = std::prev(sign_caches_.end());
    }
    if (found->revision != snapshot.revision() ||
        found->generation != application_.save_generation(window)) {
        found->signs =
            ui::line_signs(application_.session(window).buffer().save_point(), snapshot.content());
        found->revision = snapshot.revision();
        found->generation = application_.save_generation(window);
    }
    return found->signs;
}

const ui::DiagnosticLineSigns& EditorModel::diagnostic_signs(WindowId window) {
    const Buffer& buffer = application_.session(window).buffer();
    const BufferId id = buffer.id();
    const RevisionId revision = buffer.snapshot().revision();
    auto found = std::ranges::find_if(sign_caches_,
                                      [id](const SignCache& cache) { return cache.buffer == id; });
    if (found == sign_caches_.end()) {
        sign_caches_.push_back({.buffer = id, .signs = {}, .diagnostic_signs = {}});
        found = std::prev(sign_caches_.end());
    }
    if (found->diagnostics_revision != revision ||
        found->diagnostics_generation != buffer.diagnostics_generation()) {
        ui::DiagnosticLineSigns signs;
        const Text& text = buffer.snapshot().content();
        for (const Diagnostic& diagnostic : buffer.diagnostics()) {
            const ui::DiagnosticSignKind kind = [&] {
                switch (diagnostic.severity) {
                case DiagnosticSeverity::Error:
                    return ui::DiagnosticSignKind::Error;
                case DiagnosticSeverity::Warning:
                    return ui::DiagnosticSignKind::Warning;
                case DiagnosticSeverity::Information:
                    return ui::DiagnosticSignKind::Information;
                case DiagnosticSeverity::Hint:
                    return ui::DiagnosticSignKind::Hint;
                }
                return ui::DiagnosticSignKind::Error;
            }();
            signs.include(text.position(diagnostic.range.start).line, kind);
        }
        found->diagnostic_signs = std::move(signs);
        found->diagnostics_revision = revision;
        found->diagnostics_generation = buffer.diagnostics_generation();
    }
    return found->diagnostic_signs;
}

std::string EditorModel::pane_id(WindowId window) {
    return std::format("window:{}:{}", window.slot, window.generation);
}

} // namespace cind::gui
