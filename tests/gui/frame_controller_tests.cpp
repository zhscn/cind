#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "gui/frame_controller.hpp"

#include <fontconfig/fontconfig.h>
#include <skia/core/SkGraphics.h>

#include <algorithm>
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
                         FrameClock::time_point now, bool geometry_changed = false,
                         bool animate_scroll = true, bool constrain_scroll_to_cursor = true) {
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
            .animate_scroll = animate_scroll,
            .constrain_scroll_to_cursor = constrain_scroll_to_cursor,
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

TEST_CASE("frame controller reuses one prepared layout for an unchanged scene") {
    SkiaPresenter presenter("monospace", 16.0F);
    GuiFrameController controller(presenter);
    const FrameClock::time_point start{};

    PresentedFrame initial =
        controller.build(request_for(presenter, frame_scene(1, 7), 0.0F, start, true));
    PresentedFrame repeated = controller.build(
        request_for(presenter, frame_scene(1, 7), 0.0F, start + std::chrono::milliseconds(1)));

    CHECK(&initial.scene() == &repeated.scene());
    CHECK(&initial.layout() == &repeated.layout());
}

TEST_CASE("direct scroll input bypasses the spring animation") {
    SkiaPresenter presenter("monospace", 16.0F);
    GuiFrameController controller(presenter);
    const FrameClock::time_point start{};

    PresentedFrame initial =
        controller.build(request_for(presenter, frame_scene(1, 7), 0.0F, start, true));
    controller.did_present(initial);
    PresentedFrame scrolled = controller.build(request_for(
        presenter, frame_scene(1, 8), 0.75F, start + std::chrono::milliseconds(1), false, false));

    CHECK_FALSE(scrolled.animated());
    CHECK(scrolled.animation().scroll_layers.empty());
    CHECK(scrolled.animation_state().visual_scroll_top == doctest::Approx(0.0F));
}

TEST_CASE("animated document chrome follows the visual scroll transform") {
    SkiaPresenter presenter("monospace", 16.0F);
    GuiFrameController controller(presenter);
    const FrameClock::time_point start{};

    PresentedFrame initial =
        controller.build(request_for(presenter, frame_scene(3, 0), 0.0F, start, true));
    controller.did_present(initial);
    PresentedFrame target = controller.build(
        request_for(presenter, frame_scene(2, 1), 1.0F, start + std::chrono::milliseconds(1)));
    controller.did_present(target);
    PresentedFrame moving = controller.build(
        request_for(presenter, frame_scene(2, 1), 1.0F, start + std::chrono::milliseconds(20)));
    const SkiaViewPresentation target_view = presenter.view_presentation(moving.layout());

    REQUIRE(moving.view().cursor_owner == SkiaCursorOwner::Document);
    REQUIRE(moving.view().cursor_rect);
    REQUIRE(target_view.cursor_rect);
    REQUIRE(moving.view().active_line_y);
    REQUIRE(target_view.active_line_y);
    const float cursor_delta = moving.view().cursor_rect->y - target_view.cursor_rect->y;
    const float highlight_delta = *moving.view().active_line_y - *target_view.active_line_y;
    CHECK(cursor_delta == doctest::Approx(highlight_delta));
    CHECK(cursor_delta > 0.0F);
}

TEST_CASE("cursor-driven scroll cannot present the caret outside the grid") {
    SkiaPresenter presenter("monospace", 16.0F);
    const FrameClock::time_point start{};

    SUBCASE("downward motion catches up at the bottom edge") {
        GuiFrameController controller(presenter);
        PresentedFrame initial =
            controller.build(request_for(presenter, frame_scene(26, 0), 0.0F, start, true));
        controller.did_present(initial);
        PresentedFrame target = controller.build(
            request_for(presenter, frame_scene(26, 3), 3.0F, start + std::chrono::milliseconds(1)));
        controller.did_present(target);
        PresentedFrame moving = controller.build(request_for(
            presenter, frame_scene(26, 3), 3.0F, start + std::chrono::milliseconds(20)));

        REQUIRE(moving.view().cursor_rect);
        REQUIRE_FALSE(moving.animation().scroll_layers.empty());
        const float grid_bottom = moving.animation().scroll_layers.back().clip_bottom;
        const SkiaViewPresentation target_view = presenter.view_presentation(moving.layout());
        REQUIRE(target_view.cursor_rect);
        REQUIRE(target_view.active_line_y);
        const float target_bottom =
            std::max(target_view.cursor_rect->y + target_view.cursor_rect->height,
                     *target_view.active_line_y + static_cast<float>(presenter.cell_height()));
        const float maximum_lag = std::max(0.0F, grid_bottom - target_bottom) /
                                  static_cast<float>(presenter.cell_height());
        CHECK(moving.view().cursor_rect->y + moving.view().cursor_rect->height <=
              grid_bottom + 0.01F);
        CHECK(moving.animation_state().cursor_constrained);
        CHECK(moving.animation_state().visual_scroll_top >= 3.0F - maximum_lag - 0.01F);
    }

    SUBCASE("upward motion catches up at the top edge") {
        GuiFrameController controller(presenter);
        PresentedFrame initial =
            controller.build(request_for(presenter, frame_scene(1, 1), 1.0F, start, true));
        controller.did_present(initial);
        PresentedFrame target = controller.build(
            request_for(presenter, frame_scene(1, 0), 0.0F, start + std::chrono::milliseconds(1)));
        controller.did_present(target);
        PresentedFrame moving = controller.build(
            request_for(presenter, frame_scene(1, 0), 0.0F, start + std::chrono::milliseconds(20)));

        REQUIRE(moving.view().cursor_rect);
        CHECK(moving.view().cursor_rect->y >= -0.01F);
        CHECK(moving.animation_state().visual_scroll_top ==
              doctest::Approx(moving.animation_state().target_scroll_top));
    }

    SUBCASE("wheel motion retains the unconstrained spring") {
        GuiFrameController controller(presenter);
        PresentedFrame initial =
            controller.build(request_for(presenter, frame_scene(26, 0), 0.0F, start, true));
        controller.did_present(initial);
        PresentedFrame target =
            controller.build(request_for(presenter, frame_scene(26, 3), 3.0F,
                                         start + std::chrono::milliseconds(1), false, true, false));
        controller.did_present(target);
        PresentedFrame moving = controller.build(request_for(presenter, frame_scene(26, 3), 3.0F,
                                                             start + std::chrono::milliseconds(20),
                                                             false, true, false));

        CHECK_FALSE(moving.animation_state().cursor_constrained);
        CHECK(moving.animation_state().visual_scroll_top < 1.0F);
    }
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
    const SkiaPreparedScrollLayer& visible = moving.animation().scroll_layers.back();
    REQUIRE(visible.scene);
    REQUIRE(visible.layout);
    REQUIRE(visible.grid_offset_y > 0.0F);

    const float y = static_cast<float>(presenter.cell_height()) + 1.0F;
    REQUIRE(y >= visible.clip_top);
    REQUIRE(y < visible.clip_bottom);
    const SkiaLogicalPoint point{.x = static_cast<float>(presenter.cell_width()), .y = y};
    const std::optional<ViewHit> visible_hit = presenter.hit_test_view(
        *visible.layout, {.x = point.x, .y = point.y - visible.grid_offset_y},
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

TEST_CASE("scroll frames retain prepared layouts for visible keyframes") {
    SkiaPresenter presenter("monospace", 16.0F);
    GuiFrameController controller(presenter);
    const FrameClock::time_point start{};

    PresentedFrame initial =
        controller.build(request_for(presenter, frame_scene(1, 20), 0.0F, start, true));
    controller.did_present(initial);
    PresentedFrame target = controller.build(
        request_for(presenter, frame_scene(1, 21), 1.0F, start + std::chrono::milliseconds(1)));
    controller.did_present(target);
    PresentedFrame first = controller.build(
        request_for(presenter, frame_scene(1, 21), 1.0F, start + std::chrono::milliseconds(20)));
    PresentedFrame second = controller.build(
        request_for(presenter, frame_scene(1, 21), 1.0F, start + std::chrono::milliseconds(30)));

    REQUIRE(first.animation().scroll_layers.size() == 2);
    REQUIRE(second.animation().scroll_layers.size() == 2);
    for (const SkiaPreparedScrollLayer& first_layer : first.animation().scroll_layers) {
        const auto second_layer = std::ranges::find_if(
            second.animation().scroll_layers, [&](const SkiaPreparedScrollLayer& candidate) {
                return candidate.scene == first_layer.scene;
            });
        REQUIRE(second_layer != second.animation().scroll_layers.end());
        CHECK(second_layer->layout == first_layer.layout);
    }
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
