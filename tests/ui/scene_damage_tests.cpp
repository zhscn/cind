#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/scene_damage.hpp"
#include "ui/scene_layout.hpp"

using namespace cind::ui;

namespace {

Scene text_scene(std::string text, int cursor_col = 1) {
    Scene scene;
    scene.rows = 2;
    scene.cols = 12;
    scene.cursor_row = 1;
    scene.cursor_col = cursor_col;
    Region body{RegionRole::TextArea, {0, 0, 1, 12}, {}};
    body.primitives().push_back({0, 0, std::move(text), StyleClass::Text, false});
    Region status{RegionRole::StatusBar, {1, 0, 1, 12}, {}, SurfaceClass::Status};
    scene.regions = {std::move(body), std::move(status)};
    return scene;
}

} // namespace

TEST_CASE("scene damage tracks changed cells and cursor pixels independently") {
    SceneDamageTracker tracker;
    CHECK(tracker.update(text_scene("abcdef")).full_repaint);

    const SceneDamage unchanged = tracker.update(text_scene("abcdef"));
    CHECK_FALSE(unchanged.full_repaint);
    CHECK(unchanged.cell_rects.empty());
    CHECK(unchanged.cursor_cells.empty());

    const SceneDamage edited = tracker.update(text_scene("abcxef"));
    CHECK_FALSE(edited.full_repaint);
    REQUIRE(edited.cell_rects.size() == 1);
    CHECK(edited.cell_rects.front().row == 0);
    CHECK(edited.cell_rects.front().col == 3);
    CHECK(edited.cell_rects.front().rows == 1);
    CHECK(edited.cell_rects.front().cols == 1);

    const SceneDamage moved = tracker.update(text_scene("abcxef", 6));
    CHECK(moved.cell_rects.empty());
    REQUIRE(moved.cursor_cells.size() == 2);
    CHECK(moved.cursor_cells[0].column == 0);
    CHECK(moved.cursor_cells[1].column == 5);
}

TEST_CASE("scene identity metadata does not invalidate unchanged pixels") {
    SceneDamageTracker tracker;
    Scene scene = text_scene("abcdef");
    scene.regions.front().id = "editor/document";
    scene.regions.front().revision = 1;
    REQUIRE(tracker.update(scene).full_repaint);

    scene.regions.front().revision = 2;
    const SceneDamage unchanged = tracker.update(scene);
    CHECK_FALSE(unchanged.full_repaint);
    CHECK(unchanged.cell_rects.empty());
}

TEST_CASE("scene damage follows grapheme width and visual style") {
    SceneDamageTracker tracker;
    CHECK(tracker.update(text_scene("a中b")).full_repaint);

    Scene changed = text_scene("a文b");
    const SceneDamage wide = tracker.update(changed);
    REQUIRE(wide.cell_rects.size() == 1);
    CHECK(wide.cell_rects.front().col == 1);
    CHECK(wide.cell_rects.front().cols == 2);

    changed.regions.front().primitives().front().style = StyleClass::Keyword;
    const SceneDamage styled = tracker.update(changed);
    REQUIRE(styled.cell_rects.size() == 1);
    CHECK(styled.cell_rects.front().col == 0);
    CHECK(styled.cell_rects.front().cols == 4);
}

TEST_CASE("scene damage requests full repaint for geometry and broad changes") {
    SceneDamageTracker tracker;
    CHECK(tracker.update(text_scene("a")).full_repaint);
    Scene broad = text_scene("abcdefghijkl");
    broad.regions.back().primitives().push_back(
        {0, 0, "status line!", StyleClass::StatusBar, false});
    CHECK(tracker.update(broad).full_repaint);

    Scene resized = text_scene("abcdefghijkl");
    resized.cols = 13;
    resized.regions.front().rect.cols = 13;
    resized.regions.back().rect.cols = 13;
    CHECK(tracker.update(resized).full_repaint);

    tracker.reset();
    CHECK(tracker.update(resized).full_repaint);

    SceneDamageTracker offset_tracker;
    Scene shifted = text_scene("abcdefghijkl");
    CHECK(offset_tracker.update(shifted).full_repaint);
    shifted.grid_offset_rows = -0.5F;
    CHECK(offset_tracker.update(shifted).full_repaint);
}

TEST_CASE("scene damage tracks active line and overlay geometry") {
    SceneDamageTracker tracker;
    Scene scene = text_scene("line");
    scene.rows = 4;
    scene.regions.front().rect.rows = 3;
    scene.regions.back().rect.row = 3;
    scene.active_text_row = 0;
    CHECK(tracker.update(scene).full_repaint);

    scene.active_text_row.reset();
    const SceneDamage active_line = tracker.update(scene);
    REQUIRE_FALSE(active_line.full_repaint);
    REQUIRE(active_line.cell_rects.size() == 1);
    CHECK(active_line.cell_rects.front().row == 0);
    CHECK(active_line.cell_rects.front().cols == scene.cols);

    Region popup{
        RegionRole::Popup, {0, 2, 1, 8}, {}, SurfaceClass::Status, VerticalAnchor::Overlay};
    popup.primitives().push_back({0, 0, "popup", StyleClass::Popup, false});
    scene.regions.push_back(popup);
    CHECK(tracker.update(scene).full_repaint);
    scene.regions.back().rect.col = 3;
    CHECK(tracker.update(scene).full_repaint);
    scene.regions.pop_back();
    CHECK(tracker.update(scene).full_repaint);
}

TEST_CASE("scene damage requests full repaint when the footer stack changes") {
    // Footer rows carry pixel heights that differ from the cell grid, so a
    // moved footer edge (the minibuffer band opening or closing) cannot be
    // expressed as cell damage: the strip between the old and new grid clip
    // bottom would keep stale footer pixels on the row it halves.
    const auto make_scene = [](bool with_band) {
        Scene scene;
        scene.rows = 8;
        scene.cols = 20;
        scene.cursor_visible = false;
        const int text_rows = with_band ? 3 : 6;
        Region body{RegionRole::TextArea, {0, 0, text_rows, 20}, {}};
        body.primitives().push_back({0, 0, "line", StyleClass::Text, false});
        Region status{RegionRole::StatusBar, {text_rows, 0, 1, 20},  {},
                      SurfaceClass::Status,  VerticalAnchor::Bottom, "editor/modeline"};
        Region echo{RegionRole::EchoArea, {7, 0, 1, 20},          {},
                    SurfaceClass::Echo,   VerticalAnchor::Bottom, "editor/echo"};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        if (with_band) {
            Region band{RegionRole::Popup,    {4, 0, 3, 20},          {},
                        SurfaceClass::Status, VerticalAnchor::Bottom, "editor/minibuffer"};
            band.set_popup(Region::PopupContent{
                .title = "Command",
                .input = std::nullopt,
                .input_cursor = std::nullopt,
                .first_item = 0,
                .total_items = 2,
                .selected_item = std::nullopt,
                .items = {{.label = "first", .detail = "command"},
                          {.label = "second", .detail = "command"}},
            });
            scene.regions.push_back(std::move(band));
        }
        return scene;
    };

    SceneDamageTracker tracker;
    CHECK(tracker.update(make_scene(false)).full_repaint);
    CHECK_FALSE(tracker.update(make_scene(false)).full_repaint);
    // The minibuffer band reflows the frame: the modeline moves up and the
    // band claims the freed rows.
    CHECK(tracker.update(make_scene(true)).full_repaint);
    CHECK_FALSE(tracker.update(make_scene(true)).full_repaint);
    CHECK(tracker.update(make_scene(false)).full_repaint);
}

TEST_CASE("scene damage tracks structured popup list metadata") {
    Scene scene = text_scene("line");
    scene.rows = 4;
    scene.regions.front().rect.rows = 2;
    scene.regions.back().rect.row = 3;
    Region popup{
        RegionRole::Popup, {0, 2, 2, 8}, {}, SurfaceClass::Status, VerticalAnchor::Overlay};
    popup.set_popup(Region::PopupContent{
        .title = "Command",
        .input = "ab",
        .input_cursor = 2,
        .first_item = 0,
        .total_items = 20,
        .selected_item = 0,
        .items = {{.label = "first", .detail = "command"}},
    });
    scene.regions.push_back(std::move(popup));

    SceneDamageTracker tracker;
    REQUIRE(tracker.update(scene).full_repaint);
    scene.regions.back().popup()->total_items = 21;
    const SceneDamage changed = tracker.update(scene);
    CHECK_FALSE(changed.full_repaint);
    CHECK_FALSE(changed.cell_rects.empty());

    scene.regions.back().popup()->input_cursor = 1;
    const SceneDamage moved = tracker.update(scene);
    CHECK_FALSE(moved.full_repaint);
    CHECK_FALSE(moved.cell_rects.empty());

    scene.regions.back().popup()->items.front().label = "second";
    const SceneDamage relabeled = tracker.update(scene);
    CHECK_FALSE(relabeled.full_repaint);
    CHECK_FALSE(relabeled.cell_rects.empty());
}

TEST_CASE("scene damage tracks structured status content") {
    Scene scene = text_scene("line");
    scene.regions.back().set_status(Region::StatusContent{
        .path = "sample.cc",
        .line = 1,
        .column = 1,
        .line_count = 1,
        .revision = 3,
        .style_origin = ".clang-format",
        .key = "C-x",
    });

    SceneDamageTracker tracker;
    REQUIRE(tracker.update(scene).full_repaint);
    scene.regions.back().status()->key = "C-x C-s";
    const SceneDamage changed = tracker.update(scene);
    CHECK_FALSE(changed.full_repaint);
    CHECK_FALSE(changed.cell_rects.empty());
}

TEST_CASE("scene damage tracks a structured echo byte caret within one cell") {
    Scene scene;
    scene.rows = 2;
    scene.cols = 20;
    scene.cursor_row = 2;
    scene.cursor_col = 10;
    Region body{RegionRole::TextArea, {0, 0, 1, 20}, {}};
    Region echo{
        RegionRole::EchoArea, {1, 0, 1, 20}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    echo.set_echo(Region::EchoContent{.text = "search: é", .cursor_byte = 9, .key = {}});
    scene.regions = {std::move(body), std::move(echo)};

    SceneDamageTracker tracker;
    REQUIRE(tracker.update(scene).full_repaint);
    scene.regions.back().set_echo(
        Region::EchoContent{.text = "search: é", .cursor_byte = 11, .key = {}});
    const SceneDamage moved = tracker.update(scene);
    CHECK_FALSE(moved.full_repaint);
    CHECK_FALSE(moved.cell_rects.empty());

    scene.regions.back().set_echo(
        Region::EchoContent{.text = "search: x", .cursor_byte = 9, .key = {}});
    const SceneDamage edited = tracker.update(scene);
    CHECK_FALSE(edited.full_repaint);
    CHECK_FALSE(edited.cell_rects.empty());

    // The pending-key echo at the right edge is part of the painted output.
    scene.regions.back().set_echo(
        Region::EchoContent{.text = "search: x", .cursor_byte = std::nullopt, .key = "C-x"});
    const SceneDamage keyed = tracker.update(scene);
    CHECK_FALSE(keyed.full_repaint);
    CHECK_FALSE(keyed.cell_rects.empty());
}

TEST_CASE("scene vertical layout keeps footer rows complete at the viewport bottom") {
    Scene scene;
    scene.rows = 4;
    scene.cols = 10;
    scene.grid_offset_rows = -0.5F;
    scene.regions = {
        {RegionRole::TextArea, {0, 0, 2, 10}, {}},
        {RegionRole::StatusBar, {2, 0, 1, 10}, {}, SurfaceClass::Status, VerticalAnchor::Bottom},
        {RegionRole::EchoArea, {3, 0, 1, 10}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom},
    };

    const SceneVerticalLayout layout(
        scene, {.cell_height = 10.0F, .viewport_height = 35.0F, .footer_heights = {}});
    REQUIRE(layout.bottom_anchor_row());
    CHECK(layout.bottom_anchor_row().value_or(-1) == 2);
    CHECK(layout.grid_clip_bottom() == doctest::Approx(15.0F));
    CHECK(layout.row_top(0) == doctest::Approx(-5.0F));
    CHECK(layout.row_top(1) == doctest::Approx(5.0F));
    CHECK(layout.row_top(2) == doctest::Approx(15.0F));
    CHECK(layout.row_top(3) == doctest::Approx(25.0F));
    CHECK(layout.row_top(4) == doctest::Approx(35.0F));
    CHECK(layout.row_height(1) == doctest::Approx(10.0F));
    CHECK(layout.row_at(0.0F) == 0);
    CHECK(layout.row_at(14.0F) == 1);
    CHECK(layout.row_at(15.0F) == 2);
    CHECK(layout.row_at(34.0F) == 3);

    SUBCASE("footer height overrides stack from the viewport bottom") {
        const SceneVerticalLayout chrome(scene, {.cell_height = 10.0F,
                                                 .viewport_height = 60.0F,
                                                 .footer_heights = editor_footer_heights(10.0F)});
        // Footer = modeline (10 + 12) + echo (10 + 8) = 40, so the grid ends
        // at 20 and the footer rows keep their overridden pixel heights.
        CHECK(chrome.grid_clip_bottom() == doctest::Approx(20.0F));
        CHECK(chrome.row_top(2) == doctest::Approx(20.0F));
        CHECK(chrome.row_top(3) == doctest::Approx(42.0F));
        CHECK(chrome.row_top(4) == doctest::Approx(60.0F));
        CHECK(chrome.row_height(2) == doctest::Approx(22.0F));
        CHECK(chrome.row_height(3) == doctest::Approx(18.0F));
        CHECK(chrome.row_height(1) == doctest::Approx(10.0F));
        CHECK(chrome.row_at(19.0F) == 1);
        CHECK(chrome.row_at(20.0F) == 2);
        CHECK(chrome.row_at(41.0F) == 2);
        CHECK(chrome.row_at(42.0F) == 3);
        CHECK(chrome.row_at(59.0F) == 3);
    }
}

TEST_CASE("scene pixel layout reserves pane-local modelines above global chrome") {
    Scene scene;
    scene.rows = 7;
    scene.cols = 10;
    scene.panes = {{.id = "top", .rect = {0, 0, 3, 10}, .active = true},
                   {.id = "bottom", .rect = {3, 0, 3, 10}, .active = false}};
    Region top_body{
        RegionRole::TextArea, {0, 0, 2, 10}, {}, SurfaceClass::Editor, VerticalAnchor::PaneGrid};
    top_body.pane_id = "top";
    Region top_status{
        RegionRole::StatusBar, {2, 0, 1, 10}, {}, SurfaceClass::Status, VerticalAnchor::Cell};
    top_status.pane_id = "top";
    Region bottom_body{
        RegionRole::TextArea, {3, 0, 2, 10}, {}, SurfaceClass::Editor, VerticalAnchor::PaneGrid};
    bottom_body.pane_id = "bottom";
    Region bottom_status{
        RegionRole::StatusBar, {5, 0, 1, 10}, {}, SurfaceClass::Status, VerticalAnchor::Cell};
    bottom_status.pane_id = "bottom";
    Region echo{
        RegionRole::EchoArea, {6, 0, 1, 10}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    scene.regions = {std::move(top_body), std::move(top_status), std::move(bottom_body),
                     std::move(bottom_status), std::move(echo)};

    const ScenePixelLayout layout(scene,
                                  {.cell_height = 10.0F,
                                   .viewport_height = 91.0F,
                                   .footer_heights = editor_footer_heights(10.0F)},
                                  8.0F);
    const ScenePixelRect top_body_bounds = layout.region_rect(scene.regions[0]);
    const ScenePixelRect top_status_bounds = layout.region_rect(scene.regions[1]);
    const ScenePixelRect bottom_body_bounds = layout.region_rect(scene.regions[2]);
    const ScenePixelRect bottom_status_bounds = layout.region_rect(scene.regions[3]);
    const ScenePixelRect echo_bounds = layout.region_rect(scene.regions[4]);

    CHECK(layout.vertical().grid_clip_bottom() == doctest::Approx(73.0F));
    CHECK(layout.workspace_row_y(3) == doctest::Approx(36.5F));
    CHECK(top_body_bounds.y == doctest::Approx(0.0F));
    CHECK(top_body_bounds.bottom() == doctest::Approx(14.5F));
    CHECK(top_status_bounds.y == doctest::Approx(14.5F));
    CHECK(top_status_bounds.height == doctest::Approx(22.0F));
    CHECK(bottom_body_bounds.y == doctest::Approx(36.5F));
    CHECK(bottom_body_bounds.bottom() == doctest::Approx(51.0F));
    CHECK(bottom_status_bounds.y == doctest::Approx(51.0F));
    CHECK(bottom_status_bounds.bottom() == doctest::Approx(73.0F));
    CHECK(echo_bounds.y == doctest::Approx(73.0F));
    CHECK(echo_bounds.bottom() == doctest::Approx(91.0F));
}
