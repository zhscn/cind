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
        .viewport = {.top_line = 0, .left_column = 0},
        .line_signs = {.first = 0, .modified = 1},
        .tab_width = 4,
        .style_origin = "LLVM",
        .message = "",
        .preedit = "",
        .last_key = "text",
        .active_window_slot = 0,
        .active_window_generation = 1,
        .input_focus = "interaction",
        .command_loop =
            {.keymaps = {"interaction.picker", "application.global"},
             .layers = {{.name = "interaction.picker",
                         .scope = "interaction",
                         .parents = {"interaction.text"}},
                        {.name = "application.global", .scope = "global", .parents = {}}},
             .override_keymaps = {"editor.system"},
             .pending_keys = "C-x",
             .pending_keymap = "application.global",
             .repeat_count = 4,
             .last_command = "edit.insert"},
        .interaction =
            {.active = true,
             .kind = "picker",
             .prompt = "Command: ",
             .input = {},
             .input_cursor = 0,
             .history = {},
             .provider = "commands",
             .allow_custom_input = false,
             .generation = 1,
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
                     .active = true}},
        .windows = {{.window_slot = 0,
                     .window_generation = 1,
                     .view_slot = 0,
                     .view_generation = 1,
                     .buffer_slot = 0,
                     .buffer_generation = 1,
                     .active = true}},
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
    Region status{RegionRole::StatusBar,  {1, 0, 1, 10},     {}, SurfaceClass::Status,
                  VerticalAnchor::Bottom, "editor/modeline", 7};
    status.set_status(Region::StatusContent{
        .path = "sample.cc",
        .line = 1,
        .column = 4,
        .line_count = 1,
        .revision = 7,
        .style_origin = ".clang-format",
        .key = "C-x",
    });
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
    echo.set_echo(Region::EchoContent{.text = "search: x", .cursor_byte = 9});
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
            .active_line_y = 0.0F,
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
        .pixel_hash = 42,
        .animation = std::move(animation),
        .damage = {.full_repaint = scroll_frame || document_cursor_animation,
                   .damaged_cells = 1,
                   .damaged_output_pixels = 1350,
                   .output_fraction = 0.03,
                   .full_reference_match = full_reference_match,
                   .rects = {{.logical = {.x = 0.0F, .y = 0.0F, .width = 30.0F, .height = 20.0F},
                              .output = {.x = 0, .y = 0, .width = 45, .height = 30}}}},
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
    CHECK(snapshot.find("\"schema\":22") != std::string::npos);
    CHECK(snapshot.find("\"path\":\"sample.cc\"") != std::string::npos);
    CHECK(snapshot.find("\"role\":\"text-area\"") != std::string::npos);
    CHECK(snapshot.find("\"content_type\":\"popup\"") != std::string::npos);
    CHECK(snapshot.find("\"content_type\":\"status\"") != std::string::npos);
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
    CHECK(snapshot.find("\"pending_keymap\":\"application.global\"") != std::string::npos);
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

    const InspectionResponse command_loop = run_inspection_query(hub, "get editor.command_loop");
    REQUIRE(command_loop.ok);
    CHECK(command_loop.payload.find("\"last_command\":\"edit.insert\"") != std::string::npos);
    CHECK(command_loop.payload.find("\"scope\":\"interaction\"") != std::string::npos);
    CHECK(command_loop.payload.find("\"parents\":[\"interaction.text\"]") != std::string::npos);
    CHECK(command_loop.payload.find("\"override_keymaps\":[\"editor.system\"]") !=
          std::string::npos);

    const InspectionResponse interaction = run_inspection_query(hub, "get editor.interaction");
    REQUIRE(interaction.ok);
    CHECK(interaction.payload.find("\"provider\":\"commands\"") != std::string::npos);
    CHECK(interaction.payload.find("\"input_cursor\":0") != std::string::npos);

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

    const InspectionResponse windows = run_inspection_query(hub, "get editor.windows");
    REQUIRE(windows.ok);
    CHECK(windows.payload.find("\"active\":true") != std::string::npos);

    const InspectionResponse focus = run_inspection_query(hub, "get editor.focus");
    REQUIRE(focus.ok);
    CHECK(focus.payload.find("\"target\":\"interaction\"") != std::string::npos);

    const InspectionResponse view_tree = run_inspection_query(hub, "get scene.view_tree");
    REQUIRE(view_tree.ok);
    CHECK(view_tree.payload.find("\"id\":\"editor/document\"") != std::string::npos);
    CHECK(view_tree.payload.find("\"layer\":\"overlay\"") != std::string::npos);

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
    CHECK(damage.payload.find("\"output\":{\"x\":0,\"y\":0,\"width\":45") != std::string::npos);
    CHECK(damage.payload.find("\"full_reference_match\":true") != std::string::npos);

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
    CHECK(animation.find("\"active_line_y\":0") != std::string::npos);
    CHECK(animation.find("\"layers\":[{\"scroll_top\":0,\"grid_offset_y\":-10,"
                         "\"clip_top\":0,\"clip_bottom\":10}") != std::string::npos);
}

TEST_CASE("inspection reports a scroll cursor detached from the current view") {
    InspectionHub hub;
    publish_test_frame(hub, false, true, false, false, false, true, true);

    const std::shared_ptr<const FrameInspection> frame = hub.latest();
    REQUIRE(frame);
    REQUIRE(frame->violations.size() == 1);
    CHECK(frame->violations.front() == "render scroll cursor does not match current view state");
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
