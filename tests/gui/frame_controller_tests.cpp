#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "gui/frame_controller.hpp"

#include <fontconfig/fontconfig.h>
#include <skia/core/SkGraphics.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

using namespace cind::gui;
using namespace cind::ui;

int main(int argc, char** argv) {
    doctest::Context context(argc, argv);
    const int result = context.run();
    SkGraphics::PurgeAllCaches();
    FcFini();
    return result;
}

namespace {

Scene frame_scene(int cursor_row, int marker, bool popup_visible = false) {
    Scene scene;
    scene.rows = 30;
    scene.cols = 100;
    scene.cursor_row = cursor_row;
    scene.cursor_col = 3;
    scene.active_text_row = cursor_row - 1;
    Region body{RegionRole::TextArea, {0, 0, 28, scene.cols}, {},
                SurfaceClass::Editor, VerticalAnchor::Grid,   "editor/document"};
    body.set_document_mapping(
        {.first_line = static_cast<std::uint32_t>(marker), .first_display_column = 0});
    for (int row = 0; row < body.rect.rows; ++row) {
        body.primitives().push_back(
            {row, 0, std::format("{} row {}", marker, row), StyleClass::Text, false});
    }
    Region status{RegionRole::StatusBar,
                  {28, 0, 1, scene.cols},
                  {},
                  SurfaceClass::Status,
                  VerticalAnchor::Bottom};
    Region echo{RegionRole::EchoArea,
                {29, 0, 1, scene.cols},
                {},
                SurfaceClass::Echo,
                VerticalAnchor::Bottom};
    scene.regions = {std::move(body), std::move(status), std::move(echo)};
    if (popup_visible) {
        Region popup{RegionRole::Popup,    {20, 6, 2, 88},          {},
                     SurfaceClass::Status, VerticalAnchor::Overlay, "editor/overlay/popup"};
        popup.set_popup(Region::PopupContent{
            .title = "Command: ",
            .input = "edit",
            .input_cursor = 4,
            .first_item = 0,
            .total_items = 1,
            .selected_item = 0,
            .items = {{.label = "edit.undo", .detail = "command"}},
        });
        scene.regions.push_back(std::move(popup));
    }
    return scene;
}

FrameRequest request_for(SkiaPresenter& presenter, Scene scene, float scroll_top,
                         FrameClock::time_point now, bool geometry_changed = false) {
    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() * scene.rows;
    return {.scene = std::move(scene),
            .identity = {.window_slot = 1,
                         .window_generation = 1,
                         .view_slot = 2,
                         .view_generation = 1,
                         .buffer_slot = 3,
                         .buffer_generation = 1,
                         .revision = 7},
            .scroll_top = scroll_top,
            .logical_width = static_cast<float>(width),
            .logical_height = static_cast<float>(height),
            .output_width = width,
            .output_height = height,
            .display_scale = 1.0F,
            .geometry_changed = geometry_changed,
            .now = now};
}

} // namespace

TEST_CASE("presented frame exposes the animated caret used for IME placement") {
    SkiaPresenter presenter("monospace", 16.0F);
    GuiFrameController controller(presenter);
    const FrameClock::time_point start{};

    PresentedFrame initial =
        controller.build(request_for(presenter, frame_scene(1, 0), 0.0F, start, true));
    REQUIRE(initial.view().cursor_rect);
    const float initial_y = initial.view().cursor_rect->y;
    controller.did_present(initial);

    PresentedFrame moving = controller.build(
        request_for(presenter, frame_scene(2, 0), 0.0F, start + std::chrono::milliseconds(1)));
    const SkiaViewPresentation target = presenter.view_presentation(moving.layout());
    REQUIRE(moving.animated());
    REQUIRE(moving.view().cursor_rect);
    REQUIRE(target.cursor_rect);
    CHECK(moving.view().cursor_rect->y == doctest::Approx(initial_y));
    CHECK(moving.view().cursor_rect->y != doctest::Approx(target.cursor_rect->y));
    CHECK(moving.animation_state().cursor_rect->y == doctest::Approx(moving.view().cursor_rect->y));
}

TEST_CASE("presented frame hit testing follows the visible scroll layer") {
    SkiaPresenter presenter("monospace", 16.0F);
    GuiFrameController controller(presenter);
    const FrameClock::time_point start{};

    PresentedFrame initial =
        controller.build(request_for(presenter, frame_scene(1, 100), 0.0F, start, true));
    controller.did_present(initial);
    PresentedFrame target = controller.build(
        request_for(presenter, frame_scene(1, 101), 1.0F, start + std::chrono::milliseconds(1)));
    controller.did_present(target);

    PresentedFrame moving = controller.build(
        request_for(presenter, frame_scene(1, 101), 1.0F, start + std::chrono::milliseconds(101)));
    REQUIRE(moving.animation().scroll_layers.size() == 2);
    const SkiaScrollLayer& visible = moving.animation().scroll_layers.back();
    REQUIRE(visible.scene);
    REQUIRE(visible.grid_offset_y > 0.0F);

    const float y = static_cast<float>(presenter.cell_height()) + 1.0F;
    REQUIRE(y >= visible.clip_top);
    REQUIRE(y < visible.clip_bottom);
    const SkiaLogicalPoint point{.x = static_cast<float>(presenter.cell_width()), .y = y};
    const SkiaFrameLayout visible_layout =
        presenter.prepare_layout(*visible.scene, static_cast<float>(moving.output_width()),
                                 static_cast<float>(moving.output_height()));
    const std::optional<ViewHit> visible_hit = presenter.hit_test_view(
        visible_layout, {.x = point.x, .y = point.y - visible.grid_offset_y},
        SkiaHitTestScope::Grid);
    REQUIRE(visible_hit);
    const std::optional<HitTarget> expected = resolve_hit_target(*visible.scene, *visible_hit);
    const std::optional<ViewHit> direct_hit =
        presenter.hit_test_view(moving.layout(), point, SkiaHitTestScope::Grid);
    REQUIRE(direct_hit);
    const std::optional<HitTarget> direct_target = resolve_hit_target(moving.scene(), *direct_hit);
    const std::optional<HitTarget> actual = moving.hit_test(presenter, point);

    REQUIRE(expected);
    REQUIRE(direct_target);
    REQUIRE(actual);
    CHECK(actual->kind == HitTargetKind::DocumentText);
    CHECK(actual->document_line == expected->document_line);
    CHECK(actual->display_column == expected->display_column);
    CHECK(actual->document_line != direct_target->document_line);
}

TEST_CASE("presented frame gives fixed overlays priority over animated document layers") {
    SkiaPresenter presenter("monospace", 16.0F);
    GuiFrameController controller(presenter);
    const FrameClock::time_point start{};

    PresentedFrame initial =
        controller.build(request_for(presenter, frame_scene(1, 10), 0.0F, start, true));
    controller.did_present(initial);
    PresentedFrame target = controller.build(request_for(presenter, frame_scene(1, 11, true), 1.0F,
                                                         start + std::chrono::milliseconds(1)));
    controller.did_present(target);
    PresentedFrame moving = controller.build(request_for(presenter, frame_scene(1, 11, true), 1.0F,
                                                         start + std::chrono::milliseconds(40)));

    REQUIRE(moving.animation().scroll_layers.size() == 2);
    REQUIRE(moving.view().cursor_owner == SkiaCursorOwner::Popup);
    REQUIRE(moving.view().cursor_rect);
    const SkiaLogicalRect cursor = *moving.view().cursor_rect;
    const std::optional<HitTarget> hit =
        moving.hit_test(presenter, {.x = cursor.x, .y = cursor.y + 1.0F});
    REQUIRE(hit);
    CHECK(hit->kind == HitTargetKind::PopupHeader);
    CHECK(hit->view_id == "editor/overlay/popup");
}
