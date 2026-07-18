#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "gui/inspect_server.hpp"
#include "gui/inspection.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <thread>

using namespace cind;
using namespace cind::gui;
using namespace cind::ui;

namespace {

PresentationStyleSheet test_styles() {
    PresentationStyleSheet styles;
    for (PresentationTextStyle& style : styles.text) {
        style.foreground = 0xFFCDD6F4;
    }
    styles.modeline.fill(0xFFCDD6F4);
    styles.inactive_alpha = 0xB0;
    styles.secondary_alpha = 0xC8;
    return styles;
}

void publish_test_frame(InspectionHub& hub, bool row_overflow = false,
                        bool full_reference_match = true, bool popup_cursor_mismatch = false,
                        bool echo_frame = false, bool echo_cursor_mismatch = false,
                        bool scroll_frame = false, bool scroll_cursor_detached = false,
                        bool document_cursor_animation = false,
                        bool document_cursor_desynced = false) {
    const std::uint64_t event = hub.record_event({.type = "text-input",
                                                  .detail = "text=x",
                                                  .handled = true,
                                                  .repaint = true,
                                                  .revision_before = 2,
                                                  .revision_after = 3});
    EditorStateSnapshot editor{
        .path = "sample.cc",
        .revision = 3,
        .document_bytes = 6,
        .line_count = 1,
        .dirty = true,
        .caret = TextOffset{3},
        .caret_position = {.line = 0, .byte_column = 3},
        .caret_display_column = 3,
        .selection =
            {.active = true,
             .primary = 1,
             .history_depth = 2,
             .metadata = "(thing . defun)",
             .ranges = {{.anchor = TextOffset{0}, .head = TextOffset{1}, .granularity = "char"},
                        {.anchor = TextOffset{2}, .head = TextOffset{3}, .granularity = "node"}}},
        .viewport = {.top_line = 0, .left_column = 0},
        .line_signs = {.first = 0, .modified = 1},
        .tab_width = 4,
        .style_origin = "LLVM",
        .message = "",
        .preedit = "",
        .last_key = "text",
        .active_window_slot = 0,
        .active_window_generation = 1,
        .input_focus = "minibuffer",
        .input_strategy = "emacs",
        .input_state = "emacs",
        .input_cursor_shape = "beam",
        .input_state_indicator = "",
        .text_input_policy = "accept",
        .text_input_command = "edit.self-insert",
        .text_input_command_available = true,
        .selection_after_edit = "collapse",
        .input_state_handler = true,
        .input_state_on_enter = true,
        .input_state_on_exit = false,
        .position_hints = {.provider = true,
                           .items = {{.position = TextOffset{2}, .label = "1"}},
                           .error = std::nullopt},
        .command_loop =
            {.keymaps = {"interaction.picker", "application.global"},
             .layers = {{.name = "interaction.picker",
                         .scope = "minibuffer",
                         .parents = {"interaction.text"}},
                        {.name = "application.global", .scope = "global", .parents = {}}},
             .override_keymaps = {"editor.system"},
             .pending_keys = "C-x",
             .pending_keymap = "application.global",
             .pending_input_state = "",
             .repeat_count = 4,
             .register_name = "a",
             .prefix_extra = {{.name = "scope", .value = std::string("word")}},
             .prefix_text = "4 \"a scope=word",
             .last_command = "edit.insert"},
        .scripting = {.engine = "guile",
                      .version = "3.0.9",
                      .modules = {"cind command", "cind emacs", "cind toy-modal", "cind meow",
                                  "cind core"},
                      .extensions = {},
                      .command_revision = 1,
                      .scripted_commands = 29,
                      .provider_revision = 1,
                      .scripted_providers = 4,
                      .binding_revision = 1,
                      .input_state_revision = 3,
                      .scripted_input_states = 2,
                      .scripted_input_strategies = 2,
                      .mode_revision = 2,
                      .scripted_modes = 3,
                      .resource_policy_revision = 4,
                      .scripted_file_mode_rules = 2,
                      .scripted_project_providers = 3,
                      .outstanding_async_tasks = 2,
                      .last_error = std::nullopt},
        .interaction =
            {.active = true,
             .window_slot = 1,
             .window_generation = 1,
             .buffer_slot = 1,
             .buffer_generation = 1,
             .view_slot = 1,
             .view_generation = 1,
             .origin_window_slot = 0,
             .origin_window_generation = 1,
             .origin_buffer_slot = 0,
             .origin_buffer_generation = 1,
             .origin_view_slot = 0,
             .origin_view_generation = 1,
             .kind = "picker",
             .keymap = "interaction.picker",
             .input_state = "emacs",
             .prompt = "Command: ",
             .input = {},
             .input_cursor = 0,
             .history = {},
             .provider = "commands",
             .allow_custom_input = false,
             .generation = 1,
             .loading = false,
             .selected = 0,
             .error = {},
             .candidates = {{.value = "file.save", .label = "file.save", .detail = "command"}}},
        .buffers = {{.buffer_slot = 0,
                     .buffer_generation = 1,
                     .view_present = true,
                     .view_slot = 0,
                     .view_generation = 1,
                     .name = "sample.cc",
                     .resource = "/tmp/sample.cc",
                     .modified = true,
                     .active = true,
                     .saving = false,
                     .major_mode = "cind.cpp",
                     .interaction_class = "editing",
                     .initial_input_state = "emacs",
                     .things = {{.name = "defun", .definition = "cind.defun"}},
                     .location_count = 0}},
        .windows = {{.window_slot = 0,
                     .window_generation = 1,
                     .view_slot = 0,
                     .view_generation = 1,
                     .buffer_slot = 0,
                     .buffer_generation = 1,
                     .active = true,
                     .input_states = {"emacs"}}},
        .projects = {{.project_slot = 0,
                      .project_generation = 1,
                      .name = "sample",
                      .roots = {"/tmp/sample"},
                      .discovery_provider = "vcs",
                      .discovery_marker = ".git",
                      .file_count = 12,
                      .index_revision = 3,
                      .indexing = false,
                      .index_error = {}}},
        .location_at_caret = {},
        .location_navigation = {.present = true,
                                .buffer_slot = 2,
                                .buffer_generation = 3,
                                .selected_index = 4,
                                .location_count = 9},
        .background_work = false,
        .project_search_running = false,
        .quit = false,
    };
    Scene scene;
    scene.rows = 2;
    scene.cols = 10;
    scene.cursor_row = 1;
    scene.cursor_col = 4;
    scene.active_text_row =
        scroll_frame || document_cursor_animation ? std::optional(0) : std::nullopt;
    Region body{RegionRole::TextArea, {0, 0, 1, 10},     {}, SurfaceClass::Editor,
                VerticalAnchor::Grid, "editor/document", 7};
    body.set_document_mapping({.first_line = 0, .first_display_column = 0});
    body.primitives().push_back(
        {0, 0, "int", StyleClass::Keyword, false, PrimKind::Text, "line:0/byte:0"});
    body.primitives().push_back(
        {0, 2, "1", StyleClass::PositionHint, false, PrimKind::PositionHint, "hint:0", 1});
    Region status{RegionRole::StatusBar,  {1, 0, 1, 10},     {}, SurfaceClass::Status,
                  VerticalAnchor::Bottom, "editor/modeline", 7};
    status.set_status(
        ModelineContent{.segments = {{.text = "sample.cc", .group = ModelineGroup::Left},
                                     {.text = "1:4", .group = ModelineGroup::Right},
                                     {.text = "C-x", .group = ModelineGroup::Right}}});
    Region popup{RegionRole::Popup,       {0, 5, 2, 5},           {}, SurfaceClass::Status,
                 VerticalAnchor::Overlay, "editor/overlay/popup", 7};
    popup.set_popup(Region::PopupContent{
        .title = "Help",
        .input = "",
        .input_cursor = 0,
        .first_item = 0,
        .total_items = 1,
        .selected_item = 0,
        .items = {{.label = "file.save", .detail = "command"}},
    });
    Region echo{RegionRole::EchoArea,   {1, 0, 1, 10}, {}, SurfaceClass::Echo,
                VerticalAnchor::Bottom, "editor/echo", 7};
    echo.set_echo(Region::EchoContent{.text = "search: x", .cursor_byte = 9, .key = {}});
    std::optional<PopupLayoutSnapshot> popup_layout;
    std::optional<EchoLayoutSnapshot> echo_layout;
    if (echo_frame) {
        scene.regions = {body, status, echo};
        echo_layout = EchoLayoutSnapshot{
            .bounds = {.x = 0.0F, .y = 72.0F, .width = 200.0F, .height = 28.0F},
            .horizontal_scroll = 0.0F,
            .text_bytes = 9,
            .cursor_byte = 9,
            .cursor_advance = 63.0F,
            .unclamped_cursor_x = echo_cursor_mismatch ? 79.0F : 75.0F,
            .cursor_clamped = echo_cursor_mismatch,
            .cursor_rect =
                LogicalPixelRectSnapshot{.x = 75.0F, .y = 76.0F, .width = 2.0F, .height = 20.0F},
            .text = {.role = "echo",
                     .byte_count = 9,
                     .advance = 63.0F,
                     .origin = {.x = 12.0F, .y = 76.0F},
                     .shape_bounds = std::nullopt},
        };
    } else if (!document_cursor_animation) {
        scene.regions = {body, status, popup};
        popup_layout = PopupLayoutSnapshot{
            .panel_bounds = {.x = 40.0F, .y = 4.0F, .width = 120.0F, .height = 72.0F},
            .header_bounds = {.x = 40.0F, .y = 8.0F, .width = 120.0F, .height = 28.0F},
            .horizontal_scroll = 0.0F,
            .input_bytes = 0,
            .input_cursor = 0,
            .cursor_advance = 0.0F,
            .unclamped_cursor_x = popup_cursor_mismatch ? 116.0F : 112.0F,
            .cursor_clamped = popup_cursor_mismatch,
            .cursor_rect =
                LogicalPixelRectSnapshot{.x = 112.0F, .y = 12.0F, .width = 2.0F, .height = 20.0F},
            .header_text = {{.role = "prompt",
                             .byte_count = 8,
                             .advance = 70.0F,
                             .origin = {.x = 52.0F, .y = 12.0F},
                             .shape_bounds = std::nullopt},
                            {.role = "input",
                             .byte_count = 0,
                             .advance = 0.0F,
                             .origin = {.x = 112.0F, .y = 12.0F},
                             .shape_bounds = std::nullopt}},
        };
    } else {
        scene.regions = {body, status};
    }
    RenderAnimationSnapshot animation;
    if (scroll_frame) {
        animation = RenderAnimationSnapshot{
            .active = true,
            .scroll = true,
            .cursor = false,
            .cursor_owner = "popup",
            .scroll_progress = 0.5F,
            .cursor_progress = 1.0F,
            .scroll_velocity = 4.0F,
            .visual_scroll_top = 0.5F,
            .target_scroll_top = 1.0F,
            .layers = {{.scroll_top = 0.0F,
                        .grid_offset_y = -10.0F,
                        .clip_top = 0.0F,
                        .clip_bottom = 10.0F},
                       {.scroll_top = 1.0F,
                        .grid_offset_y = 10.0F,
                        .clip_top = 10.0F,
                        .clip_bottom = 68.0F}},
            .active_line_y = 10.0F,
            .cursor_rect =
                LogicalPixelRectSnapshot{
                    .x = 112.0F,
                    .y = scroll_cursor_detached ? 32.0F : 12.0F,
                    .width = 2.0F,
                    .height = 20.0F,
                },
        };
    } else if (document_cursor_animation) {
        animation = RenderAnimationSnapshot{
            .active = true,
            .scroll = false,
            .cursor = true,
            .cursor_owner = "document",
            .scroll_progress = 1.0F,
            .cursor_progress = 0.5F,
            .scroll_velocity = 0.0F,
            .visual_scroll_top = 0.0F,
            .target_scroll_top = 0.0F,
            .layers = {},
            .active_line_y = document_cursor_desynced ? 0.0F : 10.0F,
            .cursor_rect =
                LogicalPixelRectSnapshot{.x = 30.0F, .y = 10.0F, .width = 2.0F, .height = 20.0F},
        };
    }
    RenderStateSnapshot render{
        .video_driver = "wayland",
        .render_driver = "gpu",
        .window_width = 200,
        .window_height = 100,
        .output_width = 300,
        .output_height = 150,
        .display_scale = 1.5F,
        .cell_width = 10,
        .cell_height = 20,
        .rows = 2,
        .columns = 10,
        .texture_format = "ARGB8888",
        .font_family = "monospace",
        .font_size = 16.0F,
        .font_metrics = {.ascent = -15.0F,
                         .descent = 4.0F,
                         .leading = 1.0F,
                         .baseline_from_row_top = 15.0F},
        .theme = {.canvas = 0xFF1E1E1E},
        .styles = test_styles(),
        .metrics = {.modeline_extra_height = 12.0F,
                    .echo_extra_height = 8.0F,
                    .footer_padding_x = 12.0F,
                    .segment_gap = 8.0F,
                    .chip_padding_x = 10.0F,
                    .minibuffer_padding_x = 16.0F,
                    .minibuffer_detail_gap = 14.0F,
                    .cursor_stroke = 2.0F,
                    .minimum_columns = 40,
                    .minimum_rows = 6},
        .pixel_hash = 42,
        .animation = std::move(animation),
        .damage = {.full_repaint = scroll_frame || document_cursor_animation,
                   .grid_transform_changed = false,
                   .grid_translation_rows = 0.0F,
                   .damaged_cells = 1,
                   .damaged_output_pixels = 1350,
                   .output_fraction = 0.03,
                   .full_reference_match = full_reference_match,
                   .rects = {{.logical = {.x = 0.0F, .y = 0.0F, .width = 30.0F, .height = 20.0F},
                              .output = {.x = 0, .y = 0, .width = 45, .height = 30}}}},
        .timings = {},
        .document_layout =
            DocumentLayoutSnapshot{
                .bounds = {.x = 0.0F, .y = 0.0F, .width = 100.0F, .height = 20.0F},
                .cursor_row = 0,
                .cursor_column = 3,
                .cursor_advance = 30.0F,
                .grid_cursor_x = 30.0F,
                .cursor_rect =
                    LogicalPixelRectSnapshot{.x = 30.0F, .y = 0.0F, .width = 2.0F, .height = 20.0F},
                .lines = {{.row = 0,
                           .end_column = 3,
                           .origin_x = 0.0F,
                           .advance = 30.0F,
                           .run_count = 1}},
            },
        .popup_layout = std::move(popup_layout),
        .echo_layout = std::move(echo_layout),
        .primitives =
            {{.region_index = 0,
              .primitive_index = 0,
              .id = {},
              .region = {},
              .kind = {},
              .layout_bounds = {.x = 0.0F, .y = 0.0F, .width = 30.0F, .height = 20.0F},
              .shape_bounds =
                  LogicalPixelRectSnapshot{.x = 0.0F, .y = -2.0F, .width = 30.0F, .height = 24.0F},
              .paint_bounds =
                  LogicalPixelRectSnapshot{
                      .x = 1.0F, .y = 2.0F, .width = 27.0F, .height = row_overflow ? 21.0F : 14.0F},
              .draw_bounds_cross_region_clip = false,
              .row_overflow = row_overflow,
              .column_overflow = false},
             {.region_index = 2,
              .primitive_index = 1,
              .id = {},
              .region = {},
              .kind = {},
              .layout_bounds = {.x = 20.0F, .y = 40.0F, .width = 100.0F, .height = 30.0F},
              .shape_bounds = std::nullopt,
              .paint_bounds = std::nullopt,
              .draw_bounds_cross_region_clip = false,
              .row_overflow = false,
              .column_overflow = false}},
    };
    if (echo_frame) {
        render.primitives[1].primitive_index = 0;
    }
    if (document_cursor_animation) {
        render.primitives.resize(1);
    }
    hub.publish(std::move(editor), std::move(scene), std::move(render), event);
}

} // namespace

TEST_CASE("inspection snapshot exposes model, scene, render, and event state") {
    InspectionHub hub;
    publish_test_frame(hub);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    CHECK(frame->frame_id == 1);
    CHECK(frame->cause_event_sequence == 1);
    CHECK(frame->recent_events.size() == 1);
    CHECK(frame->violations.empty());

    const std::string snapshot = inspection_snapshot_json(*frame);
    CHECK(snapshot.find("\"schema\":48") != std::string::npos);
    CHECK(snapshot.find("\"styles\":{\"inactive_alpha\":176") != std::string::npos);
    CHECK(snapshot.find("\"metrics\":{\"modeline_extra_height\":12") != std::string::npos);
    CHECK(snapshot.find("\"panes\":[]") != std::string::npos);
    CHECK(snapshot.find("\"path\":\"sample.cc\"") != std::string::npos);
    CHECK(snapshot.find("\"role\":\"text-area\"") != std::string::npos);
    CHECK(snapshot.find("\"content_type\":\"popup\"") != std::string::npos);
    CHECK(snapshot.find("\"content_type\":\"status\"") != std::string::npos);
    CHECK(snapshot.find("\"group\":\"left\",\"tone\":\"normal\",\"weight\":\"regular\"") !=
          std::string::npos);
    CHECK(snapshot.find("\"view_tree\":{\"id\":\"scene\"") != std::string::npos);
    CHECK(snapshot.find("\"id\":\"scene/overlay\"") != std::string::npos);
    CHECK(snapshot.find("\"vertical_anchor\":\"bottom\"") != std::string::npos);
    CHECK(snapshot.find("\"display_scale\":1.5") != std::string::npos);
    CHECK(snapshot.find("\"grid_offset_rows\":0") != std::string::npos);
    CHECK(snapshot.find("\"top_line_offset\":0") != std::string::npos);
    CHECK(snapshot.find("\"render_driver\":\"gpu\"") != std::string::npos);
    CHECK(snapshot.find("\"scroll_velocity\":0") != std::string::npos);
    CHECK(snapshot.find("\"baseline_from_row_top\":15") != std::string::npos);
    CHECK(snapshot.find("\"layout_bounds\":{\"x\":0") != std::string::npos);
    CHECK(snapshot.find("\"damaged_output_pixels\":1350") != std::string::npos);
    CHECK(snapshot.find("\"type\":\"text-input\"") != std::string::npos);
    CHECK(snapshot.find("\"pending_keys\":\"C-x\"") != std::string::npos);
    CHECK(snapshot.find("\"file_count\":12") != std::string::npos);
    CHECK(snapshot.find("\"discovery_provider\":\"vcs\"") != std::string::npos);
    CHECK(snapshot.find("\"discovery_marker\":\".git\"") != std::string::npos);
    CHECK(snapshot.find("\"resource_policy_revision\":4") != std::string::npos);
    CHECK(snapshot.find("\"scripted_file_mode_rules\":2") != std::string::npos);
    CHECK(snapshot.find("\"scripted_project_providers\":3") != std::string::npos);
    CHECK(snapshot.find("\"outstanding_async_tasks\":2") != std::string::npos);
    CHECK(snapshot.find("\"things\":[{\"name\":\"defun\",\"definition\":\"cind.defun\"}]") !=
          std::string::npos);
    CHECK(snapshot.find("\"pending_keymap\":\"application.global\"") != std::string::npos);
    CHECK(snapshot.find("\"pending_input_state\":\"\"") != std::string::npos);
    CHECK(snapshot.find("\"register\":\"a\"") != std::string::npos);
    CHECK(snapshot.find("\"prefix_extra\":[{\"name\":\"scope\",\"value\":\"word\"}]") !=
          std::string::npos);
    CHECK(snapshot.find("\"selection\":{\"active\":true,\"primary\":1") != std::string::npos);
    CHECK(snapshot.find("\"history_depth\":2") != std::string::npos);
    CHECK(snapshot.find("\"selection_after_edit\":\"collapse\"") != std::string::npos);
    CHECK(snapshot.find("\"position_hints\":{\"provider\":true,\"items\":[{\"byte\":2,"
                        "\"label\":\"1\"}],\"error\":null}") != std::string::npos);
    CHECK(snapshot.find("\"kind\":\"position-hint\",\"selected\":false,\"span_cols\":1") !=
          std::string::npos);
    CHECK(snapshot.find("\"input_cursor\":0") != std::string::npos);
    CHECK(snapshot.find("\"popup_layout\":{\"coordinate_space\":\"logical-pixels\"") !=
          std::string::npos);
    CHECK(snapshot.find("\"document_layout\":{\"coordinate_space\":\"logical-pixels\"") !=
          std::string::npos);
    CHECK(snapshot.find("\"first_item\":0,\"total_items\":1,\"selected_item\":0") !=
          std::string::npos);
    CHECK(snapshot.find("\"violations\":[]") != std::string::npos);

    const InspectionResponse caret = run_inspection_query(hub, "get editor.caret");
    REQUIRE(caret.ok);
    CHECK(caret.payload == "{\"byte\":3,\"line\":0,\"byte_column\":3,\"display_column\":3}");

    const InspectionResponse selection = run_inspection_query(hub, "get editor.selection");
    REQUIRE(selection.ok);
    CHECK(selection.payload.find("\"metadata\":\"(thing . defun)\"") != std::string::npos);
    CHECK(selection.payload.find("\"history_depth\":2") != std::string::npos);
    CHECK(selection.payload.find("\"granularity\":\"node\"") != std::string::npos);

    const InspectionResponse position_hints =
        run_inspection_query(hub, "get editor.position_hints");
    REQUIRE(position_hints.ok);
    CHECK(position_hints.payload ==
          "{\"provider\":true,\"items\":[{\"byte\":2,\"label\":\"1\"}],\"error\":null}");

    const InspectionResponse command_loop = run_inspection_query(hub, "get editor.command_loop");
    REQUIRE(command_loop.ok);
    CHECK(command_loop.payload.find("\"last_command\":\"edit.insert\"") != std::string::npos);
    CHECK(command_loop.payload.find("\"scope\":\"minibuffer\"") != std::string::npos);
    CHECK(command_loop.payload.find("\"parents\":[\"interaction.text\"]") != std::string::npos);
    CHECK(command_loop.payload.find("\"override_keymaps\":[\"editor.system\"]") !=
          std::string::npos);

    const InspectionResponse input_state = run_inspection_query(hub, "get editor.input_state");
    REQUIRE(input_state.ok);
    CHECK(input_state.payload ==
          "{\"strategy\":\"emacs\",\"name\":\"emacs\",\"cursor_shape\":\"beam\","
          "\"indicator\":\"\",\"text_input\":\"accept\","
          "\"text_command\":\"edit.self-insert\",\"text_command_available\":true,"
          "\"selection_after_edit\":\"collapse\",\"handler\":true,\"on_enter\":true,"
          "\"on_exit\":false,\"position_hints_provider\":true}");

    const InspectionResponse scripting = run_inspection_query(hub, "get editor.scripting");
    REQUIRE(scripting.ok);
    CHECK(scripting.payload.find("\"engine\":\"guile\"") != std::string::npos);
    CHECK(scripting.payload.find("\"modules\":[\"cind command\",\"cind emacs\","
                                 "\"cind toy-modal\",\"cind meow\",\"cind core\"]") !=
          std::string::npos);
    CHECK(scripting.payload.find("\"extensions\":[]") != std::string::npos);
    CHECK(scripting.payload.find("\"command_revision\":1") != std::string::npos);
    CHECK(scripting.payload.find("\"scripted_commands\":29") != std::string::npos);
    CHECK(scripting.payload.find("\"provider_revision\":1") != std::string::npos);
    CHECK(scripting.payload.find("\"scripted_providers\":4") != std::string::npos);
    CHECK(scripting.payload.find("\"binding_revision\":1") != std::string::npos);
    CHECK(scripting.payload.find("\"input_state_revision\":3") != std::string::npos);
    CHECK(scripting.payload.find("\"scripted_input_states\":2") != std::string::npos);
    CHECK(scripting.payload.find("\"scripted_input_strategies\":2") != std::string::npos);
    CHECK(scripting.payload.find("\"last_error\":null") != std::string::npos);

    const InspectionResponse interaction = run_inspection_query(hub, "get editor.interaction");
    REQUIRE(interaction.ok);
    CHECK(interaction.payload.find("\"provider\":\"commands\"") != std::string::npos);
    CHECK(interaction.payload.find("\"keymap\":\"interaction.picker\"") != std::string::npos);
    CHECK(interaction.payload.find("\"input_state\":\"emacs\"") != std::string::npos);
    CHECK(interaction.payload.find("\"input_cursor\":0") != std::string::npos);
    CHECK(interaction.payload.find("\"history_entries\":0") != std::string::npos);
    CHECK(interaction.payload.find("\"history_index\":null") != std::string::npos);
    CHECK(interaction.payload.find("\"surface\":{\"window\":{\"slot\":1") != std::string::npos);
    CHECK(interaction.payload.find("\"origin\":{\"window\":{\"slot\":0") != std::string::npos);

    const InspectionResponse popup_layout = run_inspection_query(hub, "get render.popup_layout");
    REQUIRE(popup_layout.ok);
    CHECK(popup_layout.payload.find("\"cursor_advance\":0") != std::string::npos);
    CHECK(popup_layout.payload.find("\"role\":\"input\"") != std::string::npos);

    const InspectionResponse document_layout =
        run_inspection_query(hub, "get render.document_layout");
    REQUIRE(document_layout.ok);
    CHECK(document_layout.payload.find("\"cursor_column\":3") != std::string::npos);
    CHECK(document_layout.payload.find("\"advance\":30") != std::string::npos);

    const InspectionResponse buffers = run_inspection_query(hub, "get editor.buffers");
    REQUIRE(buffers.ok);
    CHECK(buffers.payload.find("\"name\":\"sample.cc\"") != std::string::npos);
    CHECK(buffers.payload.find("\"major_mode\":\"cind.cpp\"") != std::string::npos);
    CHECK(buffers.payload.find("\"location_count\":0") != std::string::npos);

    const InspectionResponse windows = run_inspection_query(hub, "get editor.windows");
    REQUIRE(windows.ok);
    CHECK(windows.payload.find("\"active\":true") != std::string::npos);
    CHECK(windows.payload.find("\"input_states\":[\"emacs\"]") != std::string::npos);

    const InspectionResponse projects = run_inspection_query(hub, "get editor.projects");
    REQUIRE(projects.ok);
    CHECK(projects.payload.find("\"name\":\"sample\"") != std::string::npos);
    CHECK(projects.payload.find("\"index_revision\":3") != std::string::npos);

    const InspectionResponse location = run_inspection_query(hub, "get editor.location");
    REQUIRE(location.ok);
    CHECK(location.payload == "null");

    const InspectionResponse location_navigation =
        run_inspection_query(hub, "get editor.location_navigation");
    REQUIRE(location_navigation.ok);
    CHECK(location_navigation.payload.find("\"selected_index\":4") != std::string::npos);
    CHECK(location_navigation.payload.find("\"location_count\":9") != std::string::npos);

    const InspectionResponse focus = run_inspection_query(hub, "get editor.focus");
    REQUIRE(focus.ok);
    CHECK(focus.payload.find("\"target\":\"minibuffer\"") != std::string::npos);
    CHECK(focus.payload.find("\"stack\":[{\"target\":\"window\"") != std::string::npos);

    const InspectionResponse view_tree = run_inspection_query(hub, "get scene.view_tree");
    REQUIRE(view_tree.ok);
    CHECK(view_tree.payload.find("\"id\":\"editor/document\"") != std::string::npos);
    CHECK(view_tree.payload.find("\"layer\":\"overlay\"") != std::string::npos);

    const InspectionResponse panes = run_inspection_query(hub, "get scene.panes");
    REQUIRE(panes.ok);
    CHECK(panes.payload == "[]");

    const InspectionResponse cursor = run_inspection_query(hub, "get scene.cursor");
    REQUIRE(cursor.ok);
    CHECK(cursor.payload.find("\"shape\":\"beam\"") != std::string::npos);

    const InspectionResponse pick = run_inspection_query(hub, "pick 20 10");
    REQUIRE(pick.ok);
    CHECK(pick.payload.find("\"region\":\"editor/document\"") != std::string::npos);
    CHECK(pick.payload.find("\"kind\":\"document-text\"") != std::string::npos);
    CHECK(pick.payload.find("\"document_line\":0") != std::string::npos);
    CHECK(pick.payload.find("\"style\":\"keyword\"") != std::string::npos);
    CHECK(pick.payload.find("\"paint_bounds\":{\"x\":1") != std::string::npos);

    const InspectionResponse footer_pick = run_inspection_query(hub, "pick 20 90");
    REQUIRE(footer_pick.ok);
    CHECK(footer_pick.payload.find("\"region\":\"editor/modeline\"") != std::string::npos);
    CHECK(footer_pick.payload.find("\"kind\":\"status\"") != std::string::npos);

    const InspectionResponse popup_pick = run_inspection_query(hub, "pick 70 10");
    REQUIRE(popup_pick.ok);
    CHECK(popup_pick.payload.find("\"region\":\"editor/overlay/popup\"") != std::string::npos);

    const InspectionResponse visual_popup_pick = run_inspection_query(hub, "pick 30 50");
    REQUIRE(visual_popup_pick.ok);
    CHECK(visual_popup_pick.payload.find("\"region\":\"editor/overlay/popup\"") !=
          std::string::npos);
    CHECK(visual_popup_pick.payload.find("\"kind\":\"popup-item\"") != std::string::npos);
    CHECK(visual_popup_pick.payload.find("\"local_cell\":null") != std::string::npos);

    const InspectionResponse metrics = run_inspection_query(hub, "get render.font_metrics");
    REQUIRE(metrics.ok);
    CHECK(metrics.payload.find("\"baseline_from_row_top\":15") != std::string::npos);

    const InspectionResponse animation = run_inspection_query(hub, "get render.animation");
    REQUIRE(animation.ok);
    CHECK(animation.payload.find("\"active\":false") != std::string::npos);
    CHECK(animation.payload.find("\"cursor_rect\":null") != std::string::npos);

    const InspectionResponse damage = run_inspection_query(hub, "get render.damage");
    REQUIRE(damage.ok);
    CHECK(damage.payload.find("\"full_repaint\":false") != std::string::npos);
    CHECK(damage.payload.find("\"grid_translation_rows\":0") != std::string::npos);
    CHECK(damage.payload.find("\"output\":{\"x\":0,\"y\":0,\"width\":45") != std::string::npos);
    CHECK(damage.payload.find("\"full_reference_match\":true") != std::string::npos);

    const InspectionResponse timings = run_inspection_query(hub, "get render.timings");
    REQUIRE(timings.ok);
    CHECK(timings.payload.find("\"texture_scroll_reused\":false") != std::string::npos);
    CHECK(timings.payload.find("\"texture_copy_pixels\":0") != std::string::npos);
    CHECK(timings.payload.find("\"shape_cache_hits\":0") != std::string::npos);

    const InspectionResponse primitive =
        run_inspection_query(hub, "get render.primitive.line:0/byte:0");
    REQUIRE(primitive.ok);
    CHECK(primitive.payload.find("\"shape_bounds\":{\"x\":0") != std::string::npos);
    CHECK(primitive.payload.find("\"draw_bounds_cross_region_clip\":false") != std::string::npos);
    CHECK(primitive.payload.find("\"row_overflow\":false") != std::string::npos);

    hub.record_event({.type = "unhandled",
                      .detail = "",
                      .handled = false,
                      .repaint = false,
                      .revision_before = 3,
                      .revision_after = 3});
    const InspectionResponse events = run_inspection_query(hub, "events 1");
    REQUIRE(events.ok);
    CHECK(events.payload.find("\"sequence\":2") != std::string::npos);
    CHECK(events.payload.find("\"gap\":false") != std::string::npos);
    CHECK(run_inspection_query(hub, "get event.last_sequence").payload == "2");

    for (int index = 0; index < 300; ++index) {
        hub.record_event({.type = "overflow", .detail = ""});
    }
    const InspectionResponse overflow = run_inspection_query(hub, "events 1");
    REQUIRE(overflow.ok);
    CHECK(overflow.payload.find("\"gap\":true") != std::string::npos);
    CHECK(overflow.payload.find("\"oldest_available\":") != std::string::npos);
}

TEST_CASE("inspection exposes continuous document scroll layers") {
    InspectionHub hub;
    publish_test_frame(hub, false, true, false, false, false, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    CHECK(frame->violations.empty());
    const InspectionResponse response = run_inspection_query(hub, "get render.animation");
    REQUIRE(response.ok);
    const std::string& animation = response.payload;
    CHECK(animation.find("\"visual_scroll_top\":0.5") != std::string::npos);
    CHECK(animation.find("\"cursor_owner\":\"popup\"") != std::string::npos);
    CHECK(animation.find("\"active_line_y\":10") != std::string::npos);
    CHECK(animation.find("\"layers\":[{\"scroll_top\":0,\"grid_offset_y\":-10,"
                         "\"clip_top\":0,\"clip_bottom\":10}") != std::string::npos);
}

TEST_CASE("inspection reports a scroll cursor detached from the current view") {
    InspectionHub hub;
    publish_test_frame(hub, false, true, false, false, false, true, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    REQUIRE(frame->violations.size() == 1);
    CHECK(frame->violations.front() == "render scroll cursor does not match the visual viewport");
}

TEST_CASE("inspection reports a document cursor and active line out of phase") {
    InspectionHub hub;
    publish_test_frame(hub, false, true, false, false, false, false, false, true, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    REQUIRE(frame->violations.size() == 1);
    CHECK(frame->violations.front() == "render document cursor and active line are out of phase");
}

TEST_CASE("inspection reports renderer primitives that cross scene rows") {
    InspectionHub hub;
    publish_test_frame(hub, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    REQUIRE(frame->violations.size() == 1);
    CHECK(frame->violations.front() == "render primitive 'line:0/byte:0' crosses its scene row");

    const InspectionResponse primitive =
        run_inspection_query(hub, "get render.primitive.line:0/byte:0");
    REQUIRE(primitive.ok);
    CHECK(primitive.payload.find("\"row_overflow\":true") != std::string::npos);
}

TEST_CASE("inspection reports retained raster divergence") {
    InspectionHub hub;
    publish_test_frame(hub, false, false);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    REQUIRE(frame->violations.size() == 1);
    CHECK(frame->violations.front() == "retained raster differs from full reference render");
}

TEST_CASE("inspection reports popup shaped cursor divergence") {
    InspectionHub hub;
    publish_test_frame(hub, false, true, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    REQUIRE(frame->violations.size() == 1);
    CHECK(frame->violations.front() == "render popup cursor advance disagrees with input");
}

TEST_CASE("inspection exposes echo shaped layout") {
    InspectionHub hub;
    publish_test_frame(hub, false, true, false, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    CHECK(frame->violations.empty());
    const std::string snapshot = inspection_snapshot_json(*frame);
    CHECK(snapshot.find("\"echo_layout\":{\"coordinate_space\":\"logical-pixels\"") !=
          std::string::npos);
    const InspectionResponse echo_layout = run_inspection_query(hub, "get render.echo_layout");
    REQUIRE(echo_layout.ok);
    CHECK(echo_layout.payload.find("\"cursor_byte\":9") != std::string::npos);
    CHECK(echo_layout.payload.find("\"role\":\"echo\"") != std::string::npos);
}

TEST_CASE("inspection reports echo shaped cursor divergence") {
    InspectionHub hub;
    publish_test_frame(hub, false, true, false, true, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    REQUIRE(frame->violations.size() == 1);
    CHECK(frame->violations.front() == "render echo cursor advance disagrees with text");
}

TEST_CASE("inspection server uses a private Unix socket and framed responses") {
    InspectionHub hub;
    publish_test_frame(hub);
    const std::filesystem::path socket =
        std::filesystem::temp_directory_path() /
        std::format("cind-inspection-test-{}.sock", static_cast<long>(::getpid()));
    std::error_code ignored;
    std::filesystem::remove(socket, ignored);

    {
        InspectorServer server(hub, socket);
        struct stat status{};
        REQUIRE(::stat(socket.c_str(), &status) == 0);
        CHECK((status.st_mode & (S_IRWXG | S_IRWXO)) == 0);

        const InspectionResponse response = send_inspector_request(socket, "get frame.id");
        REQUIRE(response.ok);
        CHECK(response.payload == "1");

        const InspectionResponse error = send_inspector_request(socket, "get missing.path");
        CHECK_FALSE(error.ok);
        CHECK(error.payload.find("unknown inspection path") != std::string::npos);

        auto waiting = std::async(std::launch::async,
                                  [&] { return send_inspector_request(socket, "wait-frame 1"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const InspectionResponse concurrent = send_inspector_request(socket, "get frame.id");
        REQUIRE(concurrent.ok);
        CHECK(concurrent.payload == "1");
        publish_test_frame(hub);
        const InspectionResponse update = waiting.get();
        REQUIRE(update.ok);
        CHECK(update.payload.find("\"frame_id\":2") != std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(socket));
}
