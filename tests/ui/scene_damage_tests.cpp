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
    body.prims.push_back({0, 0, std::move(text), StyleClass::Text, false});
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

TEST_CASE("scene damage follows grapheme width and visual style") {
    SceneDamageTracker tracker;
    CHECK(tracker.update(text_scene("a中b")).full_repaint);

    Scene changed = text_scene("a文b");
    const SceneDamage wide = tracker.update(changed);
    REQUIRE(wide.cell_rects.size() == 1);
    CHECK(wide.cell_rects.front().col == 1);
    CHECK(wide.cell_rects.front().cols == 2);

    changed.regions.front().prims.front().style = StyleClass::Keyword;
    const SceneDamage styled = tracker.update(changed);
    REQUIRE(styled.cell_rects.size() == 1);
    CHECK(styled.cell_rects.front().col == 0);
    CHECK(styled.cell_rects.front().cols == 4);
}

TEST_CASE("scene damage requests full repaint for geometry and broad changes") {
    SceneDamageTracker tracker;
    CHECK(tracker.update(text_scene("a")).full_repaint);
    Scene broad = text_scene("abcdefghijkl");
    broad.regions.back().prims.push_back({0, 0, "status line!", StyleClass::StatusBar, false});
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
    popup.prims.push_back({0, 0, "popup", StyleClass::Popup, false});
    scene.regions.push_back(popup);
    CHECK(tracker.update(scene).full_repaint);
    scene.regions.back().rect.col = 3;
    CHECK(tracker.update(scene).full_repaint);
    scene.regions.pop_back();
    CHECK(tracker.update(scene).full_repaint);
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
    CHECK(layout.bottom_anchor_row().value() == 2);
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
