#include "gui/editor_model.hpp"

#include "document/text.hpp"
#include "ui/char_width.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace cind::gui {

EditorModel::EditorModel(std::string path, std::optional<std::string> initial, CppIndentStyle style,
                         std::string style_origin, std::uint32_t initial_line,
                         EditorPlatformServices platform_services,
                         std::optional<std::string> init_file)
    : application_({.path = std::move(path),
                    .initial_text = std::move(initial),
                    .style = style,
                    .style_origin = std::move(style_origin),
                    .initial_line = initial_line,
                    .platform_services = std::move(platform_services),
                    .init_file = std::move(init_file)}) {
    if (!application_.has_background_work() && application_.message().empty()) {
        application_.set_message("SDL3 · Skia · C-x C-s save · C-x C-c quit · M-x commands");
    }
}

void EditorModel::layout_view(int rows, int columns, float visible_text_rows) {
    const InteractionState* interaction = application_.interaction().state();
    std::size_t popup_item_count = 0;
    std::size_t popup_capacity = 0;
    std::optional<std::size_t> popup_selection;
    if (interaction != nullptr && interaction->request.kind == InteractionKind::Picker) {
        popup_item_count = interaction->candidates.size();
        popup_capacity = ui::editor_picker_capacity;
        popup_selection = interaction->candidates.empty()
                              ? std::nullopt
                              : std::optional<std::size_t>(interaction->selected);
    } else {
        popup_item_count = application_.pending_key_hints().size();
        popup_capacity = popup_item_count;
    }

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
                 .popup_selection = std::nullopt},
                pane_view);
            pane_state.top_line = pane_view.viewport.top_line;
            pane_state.top_line_offset = pane_view.viewport.top_line_offset;
            pane_state.left_column = pane_view.viewport.left_column;
        }

        EditSession& active = application_.session();
        const DocumentSnapshot active_snapshot = active.snapshot();
        const ViewportState& active_state = active.view().viewport();
        ui::EditorSceneViewState popup_view{
            .viewport = {.top_line = active_state.top_line,
                         .top_line_offset = active_state.top_line_offset,
                         .left_column = active_state.left_column},
            .popup = popup_viewport_,
        };
        popup_view = ui::layout_editor_scene({.text = active_snapshot.content(),
                                              .caret = active.caret(),
                                              .rows = rows,
                                              .cols = columns,
                                              .visible_text_rows = visible_text_rows,
                                              .tab_width = active.style().tab_width,
                                              .reveal_caret = false,
                                              .popup_item_count = popup_item_count,
                                              .popup_capacity = popup_capacity,
                                              .popup_selection = popup_selection},
                                             popup_view);
        popup_viewport_ = popup_view.popup;
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
    };
    view = ui::layout_editor_scene({.text = snapshot.content(),
                                    .caret = session.caret(),
                                    .rows = rows,
                                    .cols = columns,
                                    .visible_text_rows = visible_text_rows,
                                    .tab_width = session.style().tab_width,
                                    .reveal_caret = application_.reveal_caret(),
                                    .popup_item_count = popup_item_count,
                                    .popup_capacity = popup_capacity,
                                    .popup_selection = popup_selection},
                                   view);
    state.top_line = view.viewport.top_line;
    state.top_line_offset = view.viewport.top_line_offset;
    state.left_column = view.viewport.left_column;
    popup_viewport_ = view.popup;
    last_rows_ = rows;
}

ui::Scene EditorModel::compose(int rows, int columns, float visible_text_rows) {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    std::string interaction_echo;
    const std::string interaction_input = application_.interaction().active()
                                              ? application_.interaction().input_text()
                                              : std::string();
    const std::size_t interaction_caret = application_.interaction().input_caret().value;
    std::optional<int> echo_cursor;
    std::optional<std::size_t> echo_cursor_byte;
    const InteractionState* interaction = application_.interaction().state();
    const std::string_view echo = [&]() -> std::string_view {
        if (interaction != nullptr && interaction->request.kind != InteractionKind::Picker) {
            interaction_echo = interaction->request.prompt + interaction_input;
            echo_cursor =
                ui::display_width(interaction->request.prompt) +
                ui::display_width(std::string_view(interaction_input).substr(0, interaction_caret));
            echo_cursor_byte = interaction->request.prompt.size() + interaction_caret;
            return interaction_echo;
        }
        return preedit_.empty() ? std::string_view(application_.message())
                                : std::string_view(preedit_);
    }();
    const std::vector<KeyBindingHint> key_hints = application_.pending_key_hints();
    std::vector<ui::EditorPopupItem> popup_items;
    std::string popup_title;
    std::optional<std::size_t> popup_selection;
    std::optional<std::string_view> popup_input;
    std::optional<std::size_t> popup_input_cursor;
    std::size_t popup_capacity = 0;
    if (interaction != nullptr && interaction->request.kind == InteractionKind::Picker) {
        popup_capacity = ui::editor_picker_capacity;
        popup_title = interaction->request.prompt;
        popup_selection = interaction->candidates.empty()
                              ? std::nullopt
                              : std::optional<std::size_t>(interaction->selected);
        popup_items.reserve(interaction->candidates.size());
        popup_input = interaction_input;
        popup_input_cursor = interaction_caret;
        for (const InteractionCandidate& candidate : interaction->candidates) {
            popup_items.push_back({.label = candidate.label, .detail = candidate.detail});
        }
    } else if (!key_hints.empty()) {
        popup_capacity = key_hints.size();
        popup_title = application_.pending_key_sequence_text() + " …";
        popup_items.reserve(key_hints.size());
        for (const KeyBindingHint& hint : key_hints) {
            const std::string_view detail = hint.detail.empty() && hint.prefix
                                                ? std::string_view("prefix")
                                                : std::string_view(hint.detail);
            popup_items.push_back({.label = hint.key, .detail = detail});
        }
    }
    const std::string pending_key = application_.pending_command_text();
    const InputStateRegistry::Definition& active_input_state = application_.input_state();
    const ViewportState& state = session.view().viewport();
    const ui::EditorSceneViewState view{
        .viewport = {.top_line = state.top_line,
                     .top_line_offset = state.top_line_offset,
                     .left_column = state.left_column},
        .popup = popup_viewport_,
    };
    const std::vector<TextRange> active_selections = session.selected_ranges();
    PositionHintProviderResult active_hint_result =
        application_.position_hints(application_.window_id());
    const std::vector<PositionHint> active_position_hints =
        active_hint_result ? std::move(*active_hint_result) : std::vector<PositionHint>{};
    const ui::EditorSceneInput active_input{.text = snapshot.content(),
                                            .tokens = application_.syntax_tokens(),
                                            .signs = signs(application_.window_id()),
                                            .caret = session.caret(),
                                            .selections = active_selections,
                                            .position_hints = active_position_hints,
                                            .rows = rows,
                                            .cols = columns,
                                            .visible_text_rows = visible_text_rows,
                                            .tab_width = session.style().tab_width,
                                            .path = application_.path(),
                                            .dirty = application_.dirty(),
                                            .revision = snapshot.revision(),
                                            .style_origin = application_.style_origin(),
                                            .last_key = application_.last_key(),
                                            .cursor_shape = active_input_state.cursor,
                                            .input_state_indicator = active_input_state.indicator,
                                            .pending_key = pending_key,
                                            .echo = echo,
                                            .echo_cursor_column = echo_cursor,
                                            .echo_cursor_byte = echo_cursor_byte,
                                            .popup_title = popup_title,
                                            .popup_items = popup_items,
                                            .popup_capacity = popup_capacity,
                                            .popup_selection = popup_selection,
                                            .popup_input = popup_input,
                                            .popup_input_cursor = popup_input_cursor};
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
        const ui::EditorSceneViewState pane_view{
            .viewport = {.top_line = pane_state.top_line,
                         .top_line_offset = pane_state.top_line_offset,
                         .left_column = pane_state.left_column},
            .popup = {},
        };
        const bool active = placement.window == application_.window_id();
        const InputStateRegistry::Definition& pane_input_state =
            application_.input_state(placement.window);
        const std::vector<TextRange> pane_selections = pane_session.selected_ranges();
        PositionHintProviderResult pane_hint_result = application_.position_hints(placement.window);
        const std::vector<PositionHint> pane_position_hints =
            pane_hint_result ? std::move(*pane_hint_result) : std::vector<PositionHint>{};
        ui::Scene pane_scene = ui::compose_editor_scene(
            {.text = pane_snapshot.content(),
             .tokens = application_.syntax_tokens(placement.window),
             .signs = signs(placement.window),
             .caret = pane_session.caret(),
             .selections = pane_selections,
             .position_hints = pane_position_hints,
             .rows = std::max(3, placement.rect.rows + 1),
             .cols = std::max(1, placement.rect.columns),
             .visible_text_rows = static_cast<float>(std::max(1, placement.rect.rows - 1)),
             .tab_width = pane_session.style().tab_width,
             .path = application_.path(placement.window),
             .dirty = application_.dirty(placement.window),
             .revision = pane_snapshot.revision(),
             .style_origin = application_.style_origin(placement.window),
             .last_key = active ? std::string_view(application_.last_key()) : std::string_view(),
             .cursor_shape = pane_input_state.cursor,
             .input_state_indicator = pane_input_state.indicator,
             .pending_key = {},
             .echo = {},
             .echo_cursor_column = std::nullopt,
             .echo_cursor_byte = std::nullopt,
             .popup_title = {},
             .popup_items = {},
             .popup_capacity = 0,
             .popup_selection = std::nullopt,
             .popup_input = std::nullopt,
             .popup_input_cursor = std::nullopt},
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
    if (application_.interaction().active() ||
        !application_.command_loop().pending_sequence().empty()) {
        return;
    }
    if (!target.document_line || (target.kind != ui::HitTargetKind::DocumentText &&
                                  target.kind != ui::HitTargetKind::DocumentGutter)) {
        return;
    }
    if (!target.pane_id.empty()) {
        for (const OpenWindowSnapshot& window : application_.open_windows()) {
            if (pane_id(window.window) == target.pane_id) {
                (void)application_.focus_window(window.window);
                break;
            }
        }
    }
    application_.reset_preferred_column();
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const Text& text = snapshot.content();
    const std::uint32_t line = std::min(*target.document_line, text.line_count() - 1);
    if (target.kind == ui::HitTargetKind::DocumentGutter || !target.display_column) {
        session.set_caret(text.line_start(line));
        application_.show_caret();
        return;
    }
    session.set_caret(ui::offset_at_display_column(
        text, {.line = line, .column = std::max(0, *target.display_column)},
        session.style().tab_width));
    application_.show_caret();
}

void EditorModel::scroll_lines(float delta) {
    EditSession& session = application_.session();
    const DocumentSnapshot snapshot = session.snapshot();
    const int last_line = static_cast<int>(snapshot.content().line_count()) - 1;
    ViewportState& viewport = session.view().viewport();
    const double position = static_cast<double>(viewport.top_line) +
                            static_cast<double>(viewport.top_line_offset) +
                            static_cast<double>(delta);
    const double clamped = std::clamp(position, 0.0, static_cast<double>(last_line));
    const double integral = std::floor(clamped);
    viewport.top_line = static_cast<std::uint32_t>(integral);
    viewport.top_line_offset = static_cast<float>(clamped - integral);
    application_.hide_caret();
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
                           .things = std::move(things),
                           .location_count = buffer.location_count});
    }
    std::vector<OpenWindowStateSnapshot> windows;
    for (const OpenWindowSnapshot& window : application_.open_windows()) {
        std::vector<std::string> input_states;
        for (const InputStateId state : runtime.views().get(window.view).input_states().stack()) {
            input_states.push_back(runtime.input_states().definition(state).name);
        }
        windows.push_back({.window_slot = window.window.slot,
                           .window_generation = window.window.generation,
                           .view_slot = window.view.slot,
                           .view_generation = window.view.generation,
                           .buffer_slot = window.buffer.slot,
                           .buffer_generation = window.buffer.generation,
                           .active = window.active,
                           .input_states = std::move(input_states)});
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
            .selection_after_edit = std::string(selection_edit_policy_name(
                application_.runtime().selection_edit_policy(focused_input_view.id()))),
            .input_state_handler = static_cast<bool>(input_state.handler),
            .input_state_on_enter = static_cast<bool>(input_state.on_enter),
            .input_state_on_exit = static_cast<bool>(input_state.on_exit),
            .position_hints = std::move(position_hints),
            .command_loop = std::move(command_state),
            .scripting = std::move(scripting_state),
            .interaction = std::move(interaction_state),
            .buffers = std::move(buffers),
            .windows = std::move(windows),
            .projects = std::move(projects),
            .location_at_caret = std::move(location_at_caret),
            .location_navigation = location_navigation,
            .background_work = application_.has_background_work(),
            .project_search_running = application_.project_search_running(),
            .quit_armed = application_.quit_armed(),
            .quit = application_.should_quit()};
}

const ui::LineSigns& EditorModel::signs(WindowId window) {
    const BufferId buffer = application_.buffer_id(window);
    const DocumentSnapshot snapshot = application_.session(window).snapshot();
    auto found = std::ranges::find_if(
        sign_caches_, [buffer](const SignCache& cache) { return cache.buffer == buffer; });
    if (found == sign_caches_.end()) {
        sign_caches_.push_back({.buffer = buffer, .signs = {}});
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

std::string EditorModel::pane_id(WindowId window) {
    return std::format("window:{}:{}", window.slot, window.generation);
}

} // namespace cind::gui
