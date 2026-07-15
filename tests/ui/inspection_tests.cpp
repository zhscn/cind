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

void publish_test_frame(InspectionHub& hub, bool row_overflow = false) {
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
    };
    Scene scene;
    scene.rows = 2;
    scene.cols = 10;
    scene.cursor_row = 1;
    scene.cursor_col = 4;
    Region body{RegionRole::TextArea, {0, 0, 1, 10}, {}};
    body.prims.push_back(
        {0, 0, "int", StyleClass::Keyword, false, PrimKind::Text, "line:0/byte:0"});
    Region status{RegionRole::StatusBar, {1, 0, 1, 10}, {}, SurfaceClass::Status};
    scene.regions = {body, status};
    RenderStateSnapshot render{
        .video_driver = "wayland",
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
        .theme = {.background = 0xFF1E1E1E},
        .pixel_hash = 42,
        .primitives =
            {{.region_index = 0,
              .primitive_index = 0,
              .id = {},
              .region = {},
              .kind = {},
              .cell_bounds = {.x = 0.0F, .y = 0.0F, .width = 30.0F, .height = 20.0F},
              .shape_bounds =
                  LogicalPixelRectSnapshot{.x = 0.0F, .y = -2.0F, .width = 30.0F, .height = 24.0F},
              .paint_bounds =
                  LogicalPixelRectSnapshot{
                      .x = 1.0F, .y = 2.0F, .width = 27.0F, .height = row_overflow ? 21.0F : 14.0F},
              .draw_bounds_cross_region_clip = false,
              .row_overflow = row_overflow,
              .column_overflow = false}},
    };
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
    CHECK(snapshot.find("\"schema\":3") != std::string::npos);
    CHECK(snapshot.find("\"path\":\"sample.cc\"") != std::string::npos);
    CHECK(snapshot.find("\"role\":\"text-area\"") != std::string::npos);
    CHECK(snapshot.find("\"display_scale\":1.5") != std::string::npos);
    CHECK(snapshot.find("\"baseline_from_row_top\":15") != std::string::npos);
    CHECK(snapshot.find("\"cell_bounds\":{\"x\":0") != std::string::npos);
    CHECK(snapshot.find("\"type\":\"text-input\"") != std::string::npos);
    CHECK(snapshot.find("\"violations\":[]") != std::string::npos);

    const InspectionResponse caret = run_inspection_query(hub, "get editor.caret");
    REQUIRE(caret.ok);
    CHECK(caret.payload == "{\"byte\":3,\"line\":0,\"byte_column\":3,\"display_column\":3}");

    const InspectionResponse pick = run_inspection_query(hub, "pick 20 10");
    REQUIRE(pick.ok);
    CHECK(pick.payload.find("\"region\":\"region:text-area\"") != std::string::npos);
    CHECK(pick.payload.find("\"style\":\"keyword\"") != std::string::npos);
    CHECK(pick.payload.find("\"paint_bounds\":{\"x\":1") != std::string::npos);

    const InspectionResponse metrics = run_inspection_query(hub, "get render.font_metrics");
    REQUIRE(metrics.ok);
    CHECK(metrics.payload.find("\"baseline_from_row_top\":15") != std::string::npos);

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
