#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "gui/skia_presenter.hpp"
#include "ui/char_width.hpp"

#include <fontconfig/fontconfig.h>
#include <skia/core/SkGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

using namespace cind::gui;
using namespace cind::ui;

int main(int argc, char** argv) {
    doctest::Context context(argc, argv);
    const int result = context.run();
    SkGraphics::PurgeAllCaches();
    FcFini();
    return result;
}

TEST_CASE("Skia presenter paints cell regions, selection, and caret offscreen") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    REQUIRE(presenter.cell_width() > 0);
    REQUIRE(presenter.cell_height() > 0);

    Scene scene;
    scene.rows = 3;
    scene.cols = 10;
    Region body{RegionRole::TextArea, {0, 0, 1, 10}, {}};
    body.primitives().push_back({0, 2, " ", StyleClass::Text, true});
    Region status{RegionRole::StatusBar, {1, 0, 1, 10}, {}, SurfaceClass::Status};
    Region echo{RegionRole::EchoArea, {2, 0, 1, 10}, {}, SurfaceClass::Echo};
    scene.regions = {body, status, echo};
    scene.cursor_row = 1;
    scene.cursor_col = 7;

    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() * scene.rows;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));

    const auto pixel = [&](int x, int y) {
        return pixels[static_cast<std::size_t>(y * width + x)];
    };
    const int mid_x = presenter.cell_width() / 2;
    const int mid_y = presenter.cell_height() / 2;

    CHECK(pixel(mid_x, mid_y) == theme.canvas);
    CHECK(pixel(presenter.cell_width() * 2 + mid_x, mid_y) == theme.selection);
    CHECK(pixel(mid_x, presenter.cell_height() + mid_y) == theme.surface);
    const std::optional<SkiaLogicalRect> cursor =
        presenter.cursor_rect(scene, static_cast<float>(width), static_cast<float>(height));
    REQUIRE(cursor);
    const SkiaLogicalRect cursor_bounds = cursor.value_or(SkiaLogicalRect{});
    CHECK(pixel(static_cast<int>(std::ceil(cursor_bounds.x)), mid_y) == theme.cursor);

    SUBCASE("fractional device scale") {
        constexpr float scale = 1.5F;
        const int scaled_width = static_cast<int>(std::ceil(static_cast<float>(width) * scale));
        const int scaled_height = static_cast<int>(std::ceil(static_cast<float>(height) * scale));
        std::vector<std::uint32_t> scaled_pixels(
            static_cast<std::size_t>(scaled_width * scaled_height));
        presenter.render(scene, scaled_width, scaled_height, scaled_pixels.data(),
                         static_cast<std::size_t>(scaled_width) * sizeof(std::uint32_t), scale);

        const auto scaled_pixel = [&](float x, float y) {
            const int pixel_x = static_cast<int>(std::floor(x * scale));
            const int pixel_y = static_cast<int>(std::floor(y * scale));
            return scaled_pixels[static_cast<std::size_t>(pixel_y * scaled_width + pixel_x)];
        };
        CHECK(scaled_pixel(static_cast<float>(presenter.cell_width() * 2 + mid_x),
                           static_cast<float>(mid_y)) == theme.selection);
        CHECK(scaled_pixel(cursor_bounds.x, static_cast<float>(mid_y)) == theme.cursor);
    }
}

TEST_CASE("Skia document caret and hit testing use the prepared shaped line") {
    SkiaPresenter presenter("monospace", 16.0F);
    Scene scene;
    scene.rows = 3;
    scene.cols = 60;
    const std::string text = "                    s.delta,";
    Region body{RegionRole::TextArea, {0, 5, 1, 55}, {}};
    body.primitives().push_back(
        {0, 0, text.substr(0, 21), StyleClass::Text, false, PrimKind::Text, "line:0/byte:0"});
    body.primitives().push_back(
        {0, 21, ".delta", StyleClass::Keyword, false, PrimKind::Text, "line:0/byte:21"});
    body.primitives().push_back(
        {0, 27, ",", StyleClass::Text, false, PrimKind::Text, "line:0/byte:27"});
    Region status{
        RegionRole::StatusBar, {1, 0, 1, 60}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
    Region echo{
        RegionRole::EchoArea, {2, 0, 1, 60}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    scene.regions = {std::move(body), std::move(status), std::move(echo)};
    const int local_column = display_width(text);
    scene.cursor_row = 1;
    scene.cursor_col = 5 + local_column + 1;

    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() * scene.rows;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    const SkiaFrameLayout layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    CHECK(presenter.view_presentation(layout).cursor_owner == SkiaCursorOwner::Document);
    SkiaRenderDiagnostics diagnostics;
    presenter.render(layout, width, height, pixels.data(), row_bytes, 1.0F, &diagnostics);

    REQUIRE(diagnostics.document_layout);
    const SkiaDocumentLayoutDiagnostics document =
        diagnostics.document_layout.value_or(SkiaDocumentLayoutDiagnostics{});
    REQUIRE(document.cursor_rect);
    REQUIRE(document.cursor_column);
    CHECK(document.cursor_column.value_or(-1) == local_column);
    REQUIRE(document.lines.size() == 1);
    CHECK(document.lines.front().run_count == 3);
    CHECK(document.cursor_advance == doctest::Approx(document.lines.front().advance));
    const SkiaLogicalRect document_cursor = document.cursor_rect.value_or(SkiaLogicalRect{});
    CHECK(document_cursor.x ==
          doctest::Approx(document.lines.front().origin_x + document.lines.front().advance));

    const CellPoint hit =
        presenter.hit_test(layout, {.x = document_cursor.x, .y = document_cursor.y + 1.0F});
    CHECK(hit.row == 0);
    CHECK(hit.column == scene.cursor_col - 1);
}

TEST_CASE("Skia presenter derives echo caret from the painted shaped text") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    SceneDamageTracker tracker;

    Scene scene;
    scene.rows = 3;
    scene.cols = 40;
    Region body{RegionRole::TextArea, {0, 0, 1, 40}, {}};
    Region status{
        RegionRole::StatusBar, {1, 0, 1, 40}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
    Region echo{
        RegionRole::EchoArea, {2, 0, 1, 40}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    const std::string text = "search: sdfasdf";
    echo.set_echo(Region::EchoContent{.text = text, .cursor_byte = text.size()});
    scene.regions = {body, status, echo};
    scene.cursor_row = 3;
    scene.cursor_col = static_cast<int>(text.size()) + 1;

    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() + static_cast<int>(presenter.status_bar_height()) +
                       static_cast<int>(presenter.echo_area_height());
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> retained(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> reference(retained.size());
    REQUIRE(tracker.update(scene).full_repaint);
    const SkiaFrameLayout layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    CHECK(presenter.view_presentation(layout).cursor_owner == SkiaCursorOwner::Echo);
    SkiaRenderDiagnostics diagnostics;
    presenter.render(layout, width, height, retained.data(), row_bytes, 1.0F, &diagnostics);
    const std::optional<SkiaLogicalRect> cursor = presenter.cursor_rect(layout);
    REQUIRE(cursor);
    const SkiaLogicalRect cursor_bounds = cursor.value_or(SkiaLogicalRect{});
    if (!diagnostics.echo_layout) {
        FAIL("echo layout diagnostics are missing");
        return;
    }
    const SkiaEchoLayoutDiagnostics& echo_diagnostics = *diagnostics.echo_layout;
    CHECK(echo_diagnostics.cursor_byte == text.size());
    CHECK(echo_diagnostics.text.byte_count == text.size());
    CHECK(cursor_bounds.x ==
          doctest::Approx(echo_diagnostics.text.origin.x + echo_diagnostics.cursor_advance));
    REQUIRE(echo_diagnostics.cursor_rect);
    const SkiaLogicalRect diagnostic_cursor =
        echo_diagnostics.cursor_rect.value_or(SkiaLogicalRect{});
    CHECK(cursor_bounds.x == doctest::Approx(diagnostic_cursor.x));

    scene.regions.back().set_echo(Region::EchoContent{.text = text, .cursor_byte = 10});
    scene.cursor_col = 11;
    const SceneDamage damage = tracker.update(scene);
    REQUIRE_FALSE(damage.full_repaint);
    const SkiaFrameLayout changed_layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    const std::vector<SkiaLogicalRect> rectangles = presenter.damage_rects(changed_layout, damage);
    CHECK(std::ranges::any_of(rectangles, [&](const SkiaLogicalRect& rect) {
        return rect.width == doctest::Approx(static_cast<float>(width));
    }));
    presenter.render_damage(changed_layout, width, height, retained.data(), row_bytes, rectangles);
    presenter.render(changed_layout, width, height, reference.data(), row_bytes);
    CHECK(retained == reference);
}

TEST_CASE("Skia presenter paints semantic change-sign colors") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);

    Scene scene;
    scene.rows = 3;
    scene.cols = 2;
    Region signs{RegionRole::ChangeSigns, {0, 0, 3, 1}, {}, SurfaceClass::Gutter};
    signs.primitives().push_back({0, 0, " ", StyleClass::SignAdded, false, PrimKind::ChangeBar});
    signs.primitives().push_back({1, 0, " ", StyleClass::SignModified, false, PrimKind::ChangeBar});
    signs.primitives().push_back(
        {2, 0, " ", StyleClass::SignDeleted, false, PrimKind::ChangeDeletion});
    scene.regions = {signs};
    scene.cursor_row = 1;
    scene.cursor_col = 2;

    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() * scene.rows;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));

    const auto pixel = [&](int x, int y) {
        return pixels[static_cast<std::size_t>(y * width + x)];
    };
    const int middle = presenter.cell_height() / 2;
    CHECK(pixel(2, middle) == theme.sign_added);
    CHECK(pixel(2, presenter.cell_height() + middle) == theme.sign_modified);
    CHECK(pixel(2, presenter.cell_height() * 2 + 1) == theme.sign_deleted);
}

TEST_CASE("Skia presenter keeps glyph ink in its scene row") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);

    Scene scene;
    scene.rows = 2;
    scene.cols = 2;
    scene.cursor_visible = false;
    Region numbers{RegionRole::LineNumbers, {0, 0, 2, 2}, {}, SurfaceClass::Gutter};
    numbers.primitives().push_back({0, 0, "1", StyleClass::Gutter, false});
    scene.regions = {numbers};

    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() * scene.rows;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    SkiaRenderDiagnostics diagnostics;
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t), 1.0F, &diagnostics);

    int first_row_ink = 0;
    int second_row_ink = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < presenter.cell_width(); ++x) {
            if (pixels[static_cast<std::size_t>(y * width + x)] != theme.canvas) {
                (y < presenter.cell_height() ? first_row_ink : second_row_ink) += 1;
            }
        }
    }

    CHECK(first_row_ink > 0);
    CHECK(second_row_ink == 0);

    REQUIRE(diagnostics.primitives.size() == 1);
    const SkiaPrimitiveRenderDiagnostics& primitive = diagnostics.primitives.front();
    REQUIRE(primitive.shape_bounds);
    REQUIRE(primitive.paint_bounds);
    const SkiaLogicalRect& paint_bounds = primitive.paint_bounds.value();
    CHECK(primitive.layout_bounds.y == doctest::Approx(0.0F));
    CHECK(primitive.layout_bounds.height == doctest::Approx(presenter.cell_height()));
    CHECK(paint_bounds.y >= primitive.layout_bounds.y);
    CHECK(paint_bounds.y + paint_bounds.height <=
          primitive.layout_bounds.y + primitive.layout_bounds.height);
    CHECK_FALSE(primitive.row_overflow);

    constexpr float scale = 1.5F;
    const int scaled_width = static_cast<int>(std::ceil(static_cast<float>(width) * scale));
    const int scaled_height = static_cast<int>(std::ceil(static_cast<float>(height) * scale));
    std::vector<std::uint32_t> scaled_pixels(
        static_cast<std::size_t>(scaled_width * scaled_height));
    SkiaRenderDiagnostics scaled_diagnostics;
    presenter.render(scene, scaled_width, scaled_height, scaled_pixels.data(),
                     static_cast<std::size_t>(scaled_width) * sizeof(std::uint32_t), scale,
                     &scaled_diagnostics);
    REQUIRE(scaled_diagnostics.primitives.size() == 1);
    REQUIRE(scaled_diagnostics.primitives.front().paint_bounds);
    CHECK_FALSE(scaled_diagnostics.primitives.front().row_overflow);
}

TEST_CASE("Skia presenter anchors complete footer rows below a partial text row") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);

    Scene scene;
    scene.rows = 3;
    scene.cols = 12;
    scene.cursor_visible = false;
    Region body{RegionRole::TextArea, {0, 0, 1, 12}, {}};
    Region status{
        RegionRole::StatusBar, {1, 0, 1, 12}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
    Region echo{
        RegionRole::EchoArea, {2, 0, 1, 12}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    status.primitives().push_back({0, 0, "ok", StyleClass::StatusBar, false});
    scene.regions = {body, status, echo};

    const int width = presenter.cell_width() * scene.cols;
    const int partial_text_height = presenter.cell_height() / 2;
    const int status_height = static_cast<int>(presenter.status_bar_height());
    const int echo_height = static_cast<int>(presenter.echo_area_height());
    const int height = partial_text_height + status_height + echo_height;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));

    const auto pixel = [&](int y) {
        return pixels[static_cast<std::size_t>(y * width + width - 1)];
    };
    CHECK(pixel(partial_text_height - 1) == theme.canvas);
    // The modeline opens with a translucent hairline over its surface.
    CHECK(pixel(partial_text_height) != theme.surface);
    CHECK(pixel(partial_text_height) != theme.canvas);
    CHECK(pixel(partial_text_height + 1) == theme.surface);
    CHECK(pixel(partial_text_height + status_height - 1) == theme.surface);
    CHECK(pixel(partial_text_height + status_height) == theme.canvas);
    CHECK(pixel(height - 1) == theme.canvas);

    SceneDamageTracker tracker;
    REQUIRE(tracker.update(scene).full_repaint);
    scene.regions[1].primitives().front().text = "ox";
    const SceneDamage damage = tracker.update(scene);
    REQUIRE_FALSE(damage.full_repaint);
    const std::vector<SkiaLogicalRect> rectangles = presenter.damage_rects(
        scene, damage, static_cast<float>(width), static_cast<float>(height));
    REQUIRE(!rectangles.empty());
    CHECK(rectangles.front().width < static_cast<float>(width));

    std::vector<std::uint32_t> reference(pixels.size());
    presenter.render_damage(scene, width, height, pixels.data(),
                            static_cast<std::size_t>(width) * sizeof(std::uint32_t), rectangles);
    presenter.render(scene, width, height, reference.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));
    CHECK(pixels == reference);
}

TEST_CASE("Skia presenter clips the top row when the bottom caret row is complete") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);

    Scene scene;
    scene.rows = 4;
    scene.cols = 12;
    scene.grid_offset_rows = -0.5F;
    scene.cursor_row = 2;
    scene.cursor_col = 12;
    Region body{RegionRole::TextArea, {0, 0, 2, 12}, {}};
    body.primitives().push_back({0, 0, "top", StyleClass::Text, false});
    body.primitives().push_back({1, 0, "bottom", StyleClass::Text, false});
    Region status{
        RegionRole::StatusBar, {2, 0, 1, 12}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
    Region echo{
        RegionRole::EchoArea, {3, 0, 1, 12}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    scene.regions = {body, status, echo};

    const int cell_height = presenter.cell_height();
    const int partial_height = cell_height / 2;
    const int width = presenter.cell_width() * scene.cols;
    const int footer_height =
        static_cast<int>(presenter.status_bar_height() + presenter.echo_area_height());
    const int height = partial_height + 2 * cell_height + footer_height;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    SkiaRenderDiagnostics diagnostics;
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t), 1.0F, &diagnostics);

    REQUIRE(diagnostics.primitives.size() == 2);
    const float half_cell = 0.5F * static_cast<float>(cell_height);
    CHECK(diagnostics.primitives[0].layout_bounds.y == doctest::Approx(-half_cell));
    CHECK(diagnostics.primitives[1].layout_bounds.y == doctest::Approx(half_cell));

    const std::optional<SkiaLogicalRect> cursor =
        presenter.cursor_rect(scene, static_cast<float>(width), static_cast<float>(height));
    REQUIRE(cursor);
    const int cursor_x = static_cast<int>(std::ceil(cursor.value_or(SkiaLogicalRect{}).x));
    int cursor_pixels = 0;
    for (int y = 0; y < height; ++y) {
        const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(cursor_x);
        cursor_pixels += pixels[index] == theme.cursor;
    }
    CHECK(cursor_pixels == cell_height);
}

TEST_CASE("Skia presenter keeps popup painting and damage independent of fractional scrolling") {
    SkiaPresenter presenter("monospace", 16.0F);
    SceneDamageTracker tracker;

    Scene scene;
    scene.rows = 5;
    scene.cols = 16;
    scene.grid_offset_rows = -0.5F;
    scene.cursor_visible = false;
    Region body{RegionRole::TextArea, {0, 0, 3, 16}, {}};
    Region status{
        RegionRole::StatusBar, {3, 0, 1, 16}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
    Region echo{
        RegionRole::EchoArea, {4, 0, 1, 16}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    Region popup{
        RegionRole::Popup, {1, 2, 2, 12}, {}, SurfaceClass::Status, VerticalAnchor::Overlay};
    popup.primitives().push_back({0, 0, "first", StyleClass::Popup, false});
    scene.regions = {body, status, echo, popup};

    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() * scene.rows;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> retained(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> reference(retained.size());
    SkiaRenderDiagnostics diagnostics;
    REQUIRE(tracker.update(scene).full_repaint);
    presenter.render(scene, width, height, retained.data(), row_bytes, 1.0F, &diagnostics);
    REQUIRE(diagnostics.primitives.size() == 1);
    CHECK(diagnostics.primitives.front().layout_bounds.y ==
          doctest::Approx(static_cast<float>(presenter.cell_height())));

    scene.regions.back().primitives().front().text = "second";
    const SceneDamage damage = tracker.update(scene);
    REQUIRE_FALSE(damage.full_repaint);
    const std::vector<SkiaLogicalRect> rectangles = presenter.damage_rects(
        scene, damage, static_cast<float>(width), static_cast<float>(height));
    REQUIRE_FALSE(rectangles.empty());
    presenter.render_damage(scene, width, height, retained.data(), row_bytes, rectangles);
    presenter.render(scene, width, height, reference.data(), row_bytes);
    CHECK(retained == reference);
}

TEST_CASE("Skia presenter gives interactive popup independent elevated layout") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    SceneDamageTracker tracker;

    Scene scene;
    scene.rows = 30;
    scene.cols = 100;
    scene.cursor_row = 30;
    scene.cursor_col = 13;
    scene.active_text_row = 4;
    Region body{RegionRole::TextArea, {0, 8, 28, 92}, {}};
    body.primitives().push_back({4, 0, "active", StyleClass::Text, false});
    Region status{
        RegionRole::StatusBar, {28, 0, 1, 100}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
    Region echo{
        RegionRole::EchoArea, {29, 0, 1, 100}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    echo.primitives().push_back({0, 0, "Command: ed", StyleClass::Message, false});
    Region popup{
        RegionRole::Popup, {20, 6, 4, 88}, {}, SurfaceClass::Status, VerticalAnchor::Overlay};
    const std::string popup_input(36, 'm');
    popup.set_popup(Region::PopupContent{
        .title = "Command: ",
        .input = popup_input,
        .input_cursor = popup_input.size(),
        .first_item = 0,
        .total_items = 3,
        .selected_item = 0,
        .items = {{.label = "edit.undo", .detail = "command"},
                  {.label = "edit.redo", .detail = "command"},
                  {.label = "edit.yank", .detail = "command"}},
    });
    scene.regions = {body, status, echo, popup};

    const int width = presenter.cell_width() * scene.cols;
    const int height = presenter.cell_height() * scene.rows;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> retained(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> reference(retained.size());
    REQUIRE(tracker.update(scene).full_repaint);
    const SkiaFrameLayout frame_layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    CHECK(presenter.view_presentation(frame_layout).cursor_owner == SkiaCursorOwner::Popup);
    SkiaRenderDiagnostics diagnostics;
    presenter.render(frame_layout, width, height, retained.data(), row_bytes, 1.0F, &diagnostics);
    CHECK_THROWS_AS(presenter.render(frame_layout, width + 1, height, retained.data(), row_bytes),
                    std::invalid_argument);
    SkiaPresenter other_presenter("monospace", 15.0F, theme);
    CHECK_THROWS_AS(other_presenter.cursor_rect(frame_layout), std::invalid_argument);

    const std::optional<SkiaLogicalRect> cursor = presenter.cursor_rect(frame_layout);
    REQUIRE(cursor);
    const SkiaLogicalRect end_cursor = cursor.value_or(SkiaLogicalRect{});
    CHECK(end_cursor.y < static_cast<float>(popup.rect.row * presenter.cell_height()));
    if (!diagnostics.popup_layout) {
        FAIL("popup layout diagnostics are missing");
        return;
    }
    const SkiaPopupLayoutDiagnostics& popup_diagnostics = *diagnostics.popup_layout;
    CHECK(popup_diagnostics.input_bytes == popup_input.size());
    CHECK(popup_diagnostics.input_cursor == popup_input.size());
    CHECK(popup_diagnostics.cursor_rect.has_value());
    CHECK(std::ranges::any_of(
        popup_diagnostics.header_text, [&](const SkiaTextLayoutDiagnostics& text) {
            return text.role == "input" && text.byte_count == popup_input.size();
        }));

    int rightmost_input_pixel = -1;
    const int cursor_top = static_cast<int>(std::floor(end_cursor.y));
    const int cursor_bottom = static_cast<int>(std::ceil(end_cursor.y + end_cursor.height));
    for (int y = cursor_top; y < cursor_bottom; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t pixel =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            if (retained[pixel] == theme.text) {
                rightmost_input_pixel = std::max(rightmost_input_pixel, x);
            }
        }
    }
    REQUIRE(rightmost_input_pixel >= 0);
    CHECK(end_cursor.x >= static_cast<float>(rightmost_input_pixel));
    CHECK(end_cursor.x - static_cast<float>(rightmost_input_pixel) <=
          static_cast<float>(presenter.cell_width()));

    scene.regions.back().popup()->input_cursor = 0;
    const SkiaFrameLayout start_layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    const std::optional<SkiaLogicalRect> start_cursor = presenter.cursor_rect(start_layout);
    REQUIRE(start_cursor);
    CHECK(start_cursor.value_or(SkiaLogicalRect{}).x < end_cursor.x);
    scene.regions.back().popup()->input_cursor = popup_input.size();
    CHECK(std::ranges::find(retained, theme.surface) != retained.end());
    CHECK(std::ranges::find(retained, theme.raised) != retained.end());

    scene.regions.back().popup()->input = "edi";
    scene.regions.back().popup()->input_cursor = 3;
    const SceneDamage damage = tracker.update(scene);
    REQUIRE_FALSE(damage.full_repaint);
    REQUIRE_FALSE(damage.cell_rects.empty());
    const SkiaFrameLayout changed_layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    const std::vector<SkiaLogicalRect> rectangles = presenter.damage_rects(changed_layout, damage);
    REQUIRE_FALSE(rectangles.empty());
    CHECK(std::ranges::any_of(rectangles, [&](const SkiaLogicalRect& rect) {
        return rect.width > static_cast<float>(presenter.cell_width() * 50);
    }));
    presenter.render_damage(changed_layout, width, height, retained.data(), row_bytes, rectangles);
    presenter.render(changed_layout, width, height, reference.data(), row_bytes);
    CHECK(retained == reference);
}

TEST_CASE("Skia damage rendering matches a full reference frame") {
    SkiaPresenter presenter("monospace", 16.0F);
    SceneDamageTracker tracker;

    const auto make_scene = [](std::string text, int cursor_col) {
        Scene scene;
        scene.rows = 3;
        scene.cols = 20;
        scene.cursor_row = 1;
        scene.cursor_col = cursor_col;
        Region body{RegionRole::TextArea, {0, 0, 1, 20}, {}};
        body.primitives().push_back({0, 0, std::move(text), StyleClass::Text, false});
        Region status{
            RegionRole::StatusBar, {1, 0, 1, 20}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        Region echo{
            RegionRole::EchoArea, {2, 0, 1, 20}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    constexpr float scale = 1.5F;
    Scene scene = make_scene("abcdef", 1);
    const float logical_width = static_cast<float>(presenter.cell_width() * scene.cols);
    const float logical_height = static_cast<float>(presenter.cell_height() * scene.rows);
    const int width = static_cast<int>(std::ceil(logical_width * scale));
    const int height = static_cast<int>(std::ceil(logical_height * scale));
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> retained(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> reference(static_cast<std::size_t>(width * height));

    REQUIRE(tracker.update(scene).full_repaint);
    presenter.render(scene, width, height, retained.data(), row_bytes, scale);

    scene = make_scene("abcxef", 6);
    const SceneDamage damage = tracker.update(scene);
    REQUIRE_FALSE(damage.full_repaint);
    const std::vector<SkiaLogicalRect> rectangles =
        presenter.damage_rects(scene, damage, logical_width, logical_height);
    REQUIRE(!rectangles.empty());
    CHECK(std::ranges::any_of(rectangles, [&](const SkiaLogicalRect& rect) {
        return rect.x > 0.0F && rect.x + rect.width == doctest::Approx(logical_width);
    }));

    presenter.render_damage(scene, width, height, retained.data(), row_bytes, rectangles, scale);
    presenter.render(scene, width, height, reference.data(), row_bytes, scale);
    CHECK(retained == reference);

    scene = make_scene("abcxef", 18);
    const SceneDamage cursor_damage = tracker.update(scene);
    REQUIRE_FALSE(cursor_damage.full_repaint);
    CHECK(cursor_damage.cell_rects.empty());
    const std::vector<SkiaLogicalRect> cursor_rectangles =
        presenter.damage_rects(scene, cursor_damage, logical_width, logical_height);
    REQUIRE_FALSE(cursor_rectangles.empty());
    presenter.render_damage(scene, width, height, retained.data(), row_bytes, cursor_rectangles,
                            scale);
    presenter.render(scene, width, height, reference.data(), row_bytes, scale);
    CHECK(retained == reference);
}

TEST_CASE("Skia document view presentation keeps caret and active line in phase") {
    const SkiaViewPresentation from{
        .cursor_owner = SkiaCursorOwner::Document,
        .cursor_rect = SkiaLogicalRect{.x = 10.0F, .y = 20.0F, .width = 2.0F, .height = 20.0F},
        .active_line_y = 20.0F,
    };
    const SkiaViewPresentation target{
        .cursor_owner = SkiaCursorOwner::Document,
        .cursor_rect = SkiaLogicalRect{.x = 14.0F, .y = 60.0F, .width = 2.0F, .height = 20.0F},
        .active_line_y = 60.0F,
    };

    const SkiaViewPresentation middle = interpolate_view_presentation(from, target, 0.5F);
    REQUIRE(middle.cursor_rect);
    REQUIRE(middle.active_line_y);
    CHECK(middle.cursor_rect->x == doctest::Approx(12.0F));
    CHECK(middle.cursor_rect->y == doctest::Approx(40.0F));
    CHECK(*middle.active_line_y == doctest::Approx(middle.cursor_rect->y));

    SkiaViewPresentation popup = target;
    popup.cursor_owner = SkiaCursorOwner::Popup;
    popup.cursor_rect = SkiaLogicalRect{.x = 80.0F, .y = 12.0F, .width = 2.0F, .height = 20.0F};
    const SkiaViewPresentation focus_change = interpolate_view_presentation(from, popup, 0.5F);
    REQUIRE(focus_change.cursor_rect);
    CHECK(focus_change.cursor_owner == SkiaCursorOwner::Popup);
    CHECK(focus_change.cursor_rect->x == doctest::Approx(80.0F));
    CHECK(focus_change.cursor_rect->y == doctest::Approx(12.0F));
}

TEST_CASE("Skia animation frames scroll only the grid") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    const auto make_scene = [](std::string first, std::string second) {
        Scene scene;
        scene.rows = 4;
        scene.cols = 12;
        scene.cursor_visible = false;
        Region body{RegionRole::TextArea, {0, 0, 2, 12}, {}};
        body.primitives().push_back({0, 0, std::move(first), StyleClass::Text, false});
        body.primitives().push_back({1, 0, std::move(second), StyleClass::Text, false});
        Region status{
            RegionRole::StatusBar, {2, 0, 1, 12}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        Region echo{
            RegionRole::EchoArea, {3, 0, 1, 12}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    const Scene source = make_scene("one", "two");
    const Scene target = make_scene("two", "three");
    const int cell_height = presenter.cell_height();
    const int width = presenter.cell_width() * target.cols;
    const int height = cell_height * target.rows;
    const float grid_bottom =
        SceneVerticalLayout(target, presenter.vertical_metrics(static_cast<float>(height)))
            .grid_clip_bottom();
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> source_pixels(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> target_pixels(source_pixels.size());
    std::vector<std::uint32_t> animated_pixels(source_pixels.size());
    presenter.render(source, width, height, source_pixels.data(), row_bytes);
    presenter.render(target, width, height, target_pixels.data(), row_bytes);

    SkiaAnimationFrame start{
        .scroll_layers = {{.scene = std::make_shared<Scene>(source),
                           .grid_offset_y = 0.0F,
                           .clip_top = 0.0F,
                           .clip_bottom = static_cast<float>(cell_height)},
                          {.scene = std::make_shared<Scene>(target),
                           .grid_offset_y = static_cast<float>(cell_height),
                           .clip_top = static_cast<float>(cell_height),
                           .clip_bottom = grid_bottom}},
        .view = {},
    };
    presenter.render_animated(target, start, width, height, animated_pixels.data(), row_bytes);
    CHECK(animated_pixels == source_pixels);

    SkiaAnimationFrame finish{
        .scroll_layers = {{.scene = std::make_shared<Scene>(target),
                           .grid_offset_y = 0.0F,
                           .clip_top = 0.0F,
                           .clip_bottom = grid_bottom}},
        .view = {},
    };
    presenter.render_animated(target, finish, width, height, animated_pixels.data(), row_bytes);
    CHECK(animated_pixels == target_pixels);

    SkiaAnimationFrame middle{
        .scroll_layers = {{.scene = std::make_shared<Scene>(source),
                           .grid_offset_y = -static_cast<float>(cell_height) / 2.0F,
                           .clip_top = 0.0F,
                           .clip_bottom = static_cast<float>(cell_height) / 2.0F},
                          {.scene = std::make_shared<Scene>(target),
                           .grid_offset_y = static_cast<float>(cell_height) / 2.0F,
                           .clip_top = static_cast<float>(cell_height) / 2.0F,
                           .clip_bottom = grid_bottom}},
        .view = {},
    };
    presenter.render_animated(target, middle, width, height, animated_pixels.data(), row_bytes);
    const SkiaFrameLayout target_layout =
        presenter.prepare_layout(target, static_cast<float>(width), static_cast<float>(height));
    const SkiaPreparedAnimationFrame prepared_middle = presenter.prepare_animation_frame(
        middle, static_cast<float>(width), static_cast<float>(height));
    std::vector<std::uint32_t> prepared_pixels(animated_pixels.size());
    presenter.render_animated(target_layout, prepared_middle, width, height, prepared_pixels.data(),
                              row_bytes);
    CHECK(prepared_pixels == animated_pixels);
    const auto pixel = [&](int x, int y) {
        return animated_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x)];
    };
    CHECK(pixel(width - 1, cell_height * 2 + cell_height / 2) == theme.surface);
    CHECK(pixel(width - 1, cell_height * 3 + cell_height / 2) == theme.canvas);
}

TEST_CASE("Skia scroll keeps the current cursor at both viewport edges") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    const auto make_scene = [](int cursor_row) {
        Scene scene;
        scene.rows = 8;
        scene.cols = 20;
        scene.cursor_row = cursor_row;
        scene.cursor_col = 5;
        Region body{RegionRole::TextArea, {0, 0, 6, 20}, {}};
        for (int row = 0; row < body.rect.rows; ++row) {
            body.primitives().push_back(
                {row, 0, std::format("row {}", row), StyleClass::Text, false});
        }
        Region status{
            RegionRole::StatusBar, {6, 0, 1, 20}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        Region echo{
            RegionRole::EchoArea, {7, 0, 1, 20}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    const auto check_edge = [&](int cursor_row, float content_offset_rows) {
        const Scene scene = make_scene(cursor_row);
        const int cell_height = presenter.cell_height();
        const int width = presenter.cell_width() * scene.cols;
        const int height = (scene.rows - 2) * cell_height +
                           static_cast<int>(std::ceil(presenter.status_bar_height() +
                                                      presenter.echo_area_height()));
        const float grid_bottom =
            SceneVerticalLayout(scene, presenter.vertical_metrics(static_cast<float>(height)))
                .grid_clip_bottom();
        const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
        const SkiaFrameLayout layout =
            presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
        const SkiaViewPresentation view = presenter.view_presentation(layout);
        const std::optional<SkiaLogicalRect>& cursor = view.cursor_rect;
        REQUIRE(cursor);
        REQUIRE(cursor->y >= 0.0F);
        REQUIRE(cursor->y + cursor->height <= grid_bottom);

        presenter.render_animated(
            scene,
            {.scroll_layers = {{.scene = std::make_shared<Scene>(scene),
                                .grid_offset_y =
                                    content_offset_rows * static_cast<float>(cell_height),
                                .clip_top = 0.0F,
                                .clip_bottom = grid_bottom}},
             .view = view},
            width, height, pixels.data(), row_bytes);

        const int cursor_x = static_cast<int>(std::floor(cursor->x));
        const int cursor_top = static_cast<int>(std::floor(cursor->y));
        const int cursor_bottom = static_cast<int>(std::ceil(cursor->y + cursor->height));
        for (int y = cursor_top; y < cursor_bottom; ++y) {
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(cursor_x);
            CHECK(pixels[index] == theme.cursor);
        }
    };

    check_edge(1, -2.5F);
    check_edge(6, 2.5F);
}

TEST_CASE("Skia scroll layers keep the leading edge populated after sustained retargeting") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    const auto make_scene = [](int first_line) {
        Scene scene;
        scene.rows = 8;
        scene.cols = 20;
        scene.cursor_visible = false;
        Region body{RegionRole::TextArea, {0, 0, 6, 20}, {}};
        for (int row = 0; row < body.rect.rows; ++row) {
            body.primitives().push_back(
                {row, 0, std::format("line {:03}", first_line + row), StyleClass::Text, true});
        }
        Region status{
            RegionRole::StatusBar, {6, 0, 1, 20}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        Region echo{
            RegionRole::EchoArea, {7, 0, 1, 20}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    const Scene lower = make_scene(496);
    const Scene upper = make_scene(497);
    const Scene target = make_scene(500);
    const int cell_height = presenter.cell_height();
    const int width = presenter.cell_width() * target.cols;
    const int height = cell_height * target.rows;
    const float grid_bottom =
        SceneVerticalLayout(target, presenter.vertical_metrics(static_cast<float>(height)))
            .grid_clip_bottom();
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));

    presenter.render_animated(
        target,
        {.scroll_layers = {{.scene = std::make_shared<Scene>(lower),
                            .grid_offset_y = -0.75F * static_cast<float>(cell_height),
                            .clip_top = 0.0F,
                            .clip_bottom = 0.25F * static_cast<float>(cell_height)},
                           {.scene = std::make_shared<Scene>(upper),
                            .grid_offset_y = 0.25F * static_cast<float>(cell_height),
                            .clip_top = 0.25F * static_cast<float>(cell_height),
                            .clip_bottom = grid_bottom}},
         .view = {}},
        width, height, pixels.data(), row_bytes);

    const int leading_edge_height = std::max(1, cell_height / 4);
    bool leading_edge_has_document = false;
    for (int y = 0; y < leading_edge_height; ++y) {
        const auto first =
            pixels.begin() + static_cast<std::ptrdiff_t>(y) * static_cast<std::ptrdiff_t>(width);
        const auto last = first + width;
        leading_edge_has_document =
            leading_edge_has_document || std::ranges::find(first, last, theme.selection) != last;
    }
    CHECK(leading_edge_has_document);
}

TEST_CASE("Skia scroll layer handoff keeps transient active line continuous") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    const auto make_scene = [](int first_line, int active_row) {
        Scene scene;
        scene.rows = 8;
        scene.cols = 20;
        scene.cursor_visible = false;
        scene.active_text_row = active_row;
        Region body{RegionRole::TextArea, {0, 0, 6, 20}, {}};
        for (int row = 0; row < body.rect.rows; ++row) {
            body.primitives().push_back(
                {row, 0, std::format("line {:03}", first_line + row), StyleClass::Text, false});
        }
        Region status{
            RegionRole::StatusBar, {6, 0, 1, 20}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        Region echo{
            RegionRole::EchoArea, {7, 0, 1, 20}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    const Scene lower = make_scene(0, 4);
    const Scene middle = make_scene(1, 4);
    const Scene target = make_scene(2, 1);
    const int cell_height = presenter.cell_height();
    const int width = presenter.cell_width() * target.cols;
    const int height = cell_height * target.rows;
    const float grid_bottom =
        SceneVerticalLayout(target, presenter.vertical_metrics(static_cast<float>(height)))
            .grid_clip_bottom();
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> before(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> after(before.size());
    const float active_line_y = 2.0F * static_cast<float>(cell_height);

    presenter.render_animated(
        target,
        {.scroll_layers = {{.scene = std::make_shared<Scene>(lower),
                            .grid_offset_y = -0.999F * static_cast<float>(cell_height),
                            .clip_top = 0.0F,
                            .clip_bottom = 0.001F * static_cast<float>(cell_height)},
                           {.scene = std::make_shared<Scene>(middle),
                            .grid_offset_y = 0.001F * static_cast<float>(cell_height),
                            .clip_top = 0.001F * static_cast<float>(cell_height),
                            .clip_bottom = grid_bottom}},
         .view = {.cursor_owner = SkiaCursorOwner::None,
                  .cursor_rect = std::nullopt,
                  .active_line_y = active_line_y}},
        width, height, before.data(), row_bytes);
    presenter.render_animated(
        target,
        {.scroll_layers = {{.scene = std::make_shared<Scene>(middle),
                            .grid_offset_y = -0.001F * static_cast<float>(cell_height),
                            .clip_top = 0.0F,
                            .clip_bottom = 0.999F * static_cast<float>(cell_height)},
                           {.scene = std::make_shared<Scene>(target),
                            .grid_offset_y = 0.999F * static_cast<float>(cell_height),
                            .clip_top = 0.999F * static_cast<float>(cell_height),
                            .clip_bottom = grid_bottom}},
         .view = {.cursor_owner = SkiaCursorOwner::None,
                  .cursor_rect = std::nullopt,
                  .active_line_y = active_line_y}},
        width, height, after.data(), row_bytes);

    const int sample_x = width - 2;
    const int sample_y = static_cast<int>(active_line_y) + cell_height / 2;
    const auto pixel = [width, sample_x, sample_y](const std::vector<std::uint32_t>& pixels) {
        return pixels[static_cast<std::size_t>(sample_y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(sample_x)];
    };
    CHECK(pixel(before) == theme.active_line);
    CHECK(pixel(after) == theme.active_line);
    std::size_t changed_pixels = 0;
    for (std::size_t index = 0; index < before.size(); ++index) {
        changed_pixels += before[index] != after[index] ? 1 : 0;
    }
    CHECK(changed_pixels < before.size() / 50);
}

TEST_CASE("Skia line-number emphasis follows the animated document caret midpoint") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    Scene scene;
    scene.rows = 5;
    scene.cols = 12;
    scene.cursor_visible = false;
    scene.active_text_row = 1;
    Region numbers{RegionRole::LineNumbers, {0, 0, 3, 2}, {}, SurfaceClass::Gutter};
    numbers.primitives().push_back({0, 0, "1", StyleClass::Gutter, false});
    numbers.primitives().push_back({1, 0, "2", StyleClass::Gutter, false});
    numbers.primitives().push_back({2, 0, "3", StyleClass::Gutter, false});
    Region body{RegionRole::TextArea, {0, 2, 3, 10}, {}};
    Region status{
        RegionRole::StatusBar, {3, 0, 1, 12}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
    Region echo{
        RegionRole::EchoArea, {4, 0, 1, 12}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
    scene.regions = {std::move(numbers), std::move(body), std::move(status), std::move(echo)};

    const int cell_width = presenter.cell_width();
    const int cell_height = presenter.cell_height();
    const int width = cell_width * scene.cols;
    const int height =
        3 * cell_height +
        static_cast<int>(std::ceil(presenter.status_bar_height() + presenter.echo_area_height()));
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    const auto render_at = [&](float active_line_y) {
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
        presenter.render_animated(scene,
                                  {.scroll_layers = {},
                                   .view = {.cursor_owner = SkiaCursorOwner::None,
                                            .cursor_rect = std::nullopt,
                                            .active_line_y = active_line_y}},
                                  width, height, pixels.data(), row_bytes);
        return pixels;
    };
    const auto count_text_pixels = [&](const std::vector<std::uint32_t>& pixels, int row) {
        std::size_t count = 0;
        for (int y = row * cell_height; y < (row + 1) * cell_height; ++y) {
            const auto first = pixels.begin() +
                               static_cast<std::ptrdiff_t>(y) * static_cast<std::ptrdiff_t>(width);
            count +=
                static_cast<std::size_t>(std::count(first, first + 2 * cell_width, theme.text));
        }
        return count;
    };

    const std::vector<std::uint32_t> before = render_at(0.25F * static_cast<float>(cell_height));
    const std::vector<std::uint32_t> after = render_at(0.75F * static_cast<float>(cell_height));
    CHECK(count_text_pixels(before, 0) > 0);
    CHECK(count_text_pixels(before, 1) == 0);
    CHECK(count_text_pixels(after, 0) == 0);
    CHECK(count_text_pixels(after, 1) > 0);
}

TEST_CASE("Skia presenter lays the modeline out from structured status content") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    SceneDamageTracker tracker;

    const auto make_scene = [](std::string key, bool dirty) {
        Scene scene;
        scene.rows = 4;
        scene.cols = 40;
        scene.cursor_visible = false;
        Region body{RegionRole::TextArea, {0, 0, 2, 40}, {}};
        Region status{
            RegionRole::StatusBar, {2, 0, 1, 40}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        status.set_status(Region::StatusContent{
            .path = "src/ui/editor_scene.cpp",
            .dirty = dirty,
            .line = 12,
            .column = 5,
            .line_count = 48,
            .revision = 7,
            .style_origin = ".clang-format",
            .key = std::move(key),
        });
        Region echo{
            RegionRole::EchoArea, {3, 0, 1, 40}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    Scene scene = make_scene("C-x", true);
    const int width = presenter.cell_width() * scene.cols;
    const int height =
        presenter.cell_height() * 2 +
        static_cast<int>(presenter.status_bar_height() + presenter.echo_area_height());
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> retained(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> reference(retained.size());
    REQUIRE(tracker.update(scene).full_repaint);
    presenter.render(scene, width, height, retained.data(), row_bytes);

    // The dirty dot and the key chip only exist in the structured layout.
    CHECK(std::ranges::find(retained, theme.accent) != retained.end());
    CHECK(std::ranges::find(retained, theme.raised) != retained.end());

    scene = make_scene("C-x C-s", false);
    const SceneDamage damage = tracker.update(scene);
    REQUIRE_FALSE(damage.full_repaint);
    const std::vector<SkiaLogicalRect> rectangles = presenter.damage_rects(
        scene, damage, static_cast<float>(width), static_cast<float>(height));
    REQUIRE_FALSE(rectangles.empty());
    // Modeline segments move independently of the cell grid, so any status
    // damage repaints the full bar.
    CHECK(std::ranges::any_of(rectangles, [&](const SkiaLogicalRect& rect) {
        return rect.width == doctest::Approx(static_cast<float>(width));
    }));
    presenter.render_damage(scene, width, height, retained.data(), row_bytes, rectangles);
    presenter.render(scene, width, height, reference.data(), row_bytes);
    CHECK(retained == reference);
}

TEST_CASE("Skia workspace distinguishes active pane chrome and paints dividers") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    Scene scene;
    scene.rows = 4;
    scene.cols = 21;
    scene.cursor_visible = false;
    scene.panes = {{.id = "window:0:1", .rect = {0, 0, 3, 10}, .active = true},
                   {.id = "window:1:1", .rect = {0, 11, 3, 10}, .active = false}};
    scene.dividers = {{.id = "workspace/divider/0",
                       .axis = DividerAxis::Vertical,
                       .position = 10,
                       .start = 0,
                       .length = 3}};

    const auto pane_region = [](RegionRole role, Rect rect, std::string id, std::string pane,
                                bool active) {
        Region region{role,
                      rect,
                      {},
                      role == RegionRole::StatusBar ? SurfaceClass::Status : SurfaceClass::Editor,
                      role == RegionRole::StatusBar ? VerticalAnchor::Cell
                                                    : VerticalAnchor::PaneGrid,
                      std::move(id)};
        region.pane_id = std::move(pane);
        region.active = active;
        return region;
    };
    Region active_body =
        pane_region(RegionRole::TextArea, {0, 0, 2, 10}, "active/document", "window:0:1", true);
    active_body.set_document_mapping({.first_line = 0, .first_display_column = 0});
    active_body.primitives().push_back({0, 0, "active", StyleClass::Text, false});
    Region inactive_body =
        pane_region(RegionRole::TextArea, {0, 11, 2, 10}, "inactive/document", "window:1:1", false);
    inactive_body.set_document_mapping({.first_line = 0, .first_display_column = 0});
    inactive_body.primitives().push_back({0, 0, "idle", StyleClass::Text, false});
    Region active_status =
        pane_region(RegionRole::StatusBar, {2, 0, 1, 10}, "active/modeline", "window:0:1", true);
    active_status.set_status({.path = "active.cc",
                              .dirty = false,
                              .line = 1,
                              .column = 1,
                              .line_count = 1,
                              .revision = 0,
                              .style_origin = {},
                              .key = {}});
    Region inactive_status = pane_region(RegionRole::StatusBar, {2, 11, 1, 10}, "inactive/modeline",
                                         "window:1:1", false);
    inactive_status.set_status({.path = "idle.cc",
                                .dirty = false,
                                .line = 1,
                                .column = 1,
                                .line_count = 1,
                                .revision = 0,
                                .style_origin = {},
                                .key = {}});
    Region echo{RegionRole::EchoArea, {3, 0, 1, scene.cols},  {},
                SurfaceClass::Echo,   VerticalAnchor::Bottom, "editor/echo"};
    echo.set_echo({.text = "window split right", .cursor_byte = std::nullopt});
    scene.regions = {std::move(active_body), std::move(inactive_body), std::move(active_status),
                     std::move(inactive_status), std::move(echo)};

    SceneDamageTracker tracker;
    REQUIRE(tracker.update(scene).full_repaint);
    const int width = presenter.cell_width() * scene.cols;
    const int height =
        presenter.cell_height() * 2 + presenter.cell_height() / 2 +
        static_cast<int>(presenter.status_bar_height() + presenter.echo_area_height());
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));
    const auto pixel = [&](int x, int y) {
        return pixels[static_cast<std::size_t>(y * width + x)];
    };
    const int workspace_bottom = height - static_cast<int>(presenter.echo_area_height());
    const int modeline_y = workspace_bottom - 2;
    CHECK(pixel(presenter.cell_width() * 9, modeline_y) == theme.surface);
    CHECK(pixel(presenter.cell_width() * 20, modeline_y) == theme.inactive_surface);
    const int divider_x = presenter.cell_width() * 10 + presenter.cell_width() / 2;
    CHECK(pixel(divider_x, presenter.cell_height() / 2) != theme.canvas);

    const SkiaFrameLayout layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    const std::optional<ViewHit> hit =
        presenter.hit_test_view(layout, {.x = static_cast<float>(presenter.cell_width() * 12),
                                         .y = static_cast<float>(presenter.cell_height() / 2)});
    REQUIRE(hit);
    const std::optional<HitTarget> target = resolve_hit_target(scene, *hit);
    REQUIRE(target);
    CHECK(target->pane_id == "window:1:1");

    scene.regions[0].primitives()[0].text = "changed";
    Region::StatusContent* status = scene.regions[2].status();
    REQUIRE(status != nullptr);
    status->key = "C-x";
    const SceneDamage damage = tracker.update(scene);
    REQUIRE_FALSE(damage.full_repaint);
    const std::vector<SkiaLogicalRect> rectangles = presenter.damage_rects(
        scene, damage, static_cast<float>(width), static_cast<float>(height));
    REQUIRE_FALSE(rectangles.empty());
    std::vector<std::uint32_t> reference(pixels.size());
    presenter.render_damage(scene, width, height, pixels.data(),
                            static_cast<std::size_t>(width) * sizeof(std::uint32_t), rectangles);
    presenter.render(scene, width, height, reference.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));
    CHECK(pixels == reference);
}

TEST_CASE("Skia horizontal pane modelines use pane pixel boundaries") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    Scene scene;
    scene.rows = 7;
    scene.cols = 10;
    scene.cursor_visible = false;
    scene.panes = {{.id = "window:0:1", .rect = {0, 0, 3, 10}, .active = true},
                   {.id = "window:1:1", .rect = {3, 0, 3, 10}, .active = false}};
    scene.dividers = {{.id = "workspace/divider/0",
                       .axis = DividerAxis::Horizontal,
                       .position = 3,
                       .start = 0,
                       .length = 10}};

    const auto status_region = [](Rect rect, std::string id, std::string pane, bool active) {
        Region region{RegionRole::StatusBar, rect,         {}, SurfaceClass::Status,
                      VerticalAnchor::Cell,  std::move(id)};
        region.pane_id = std::move(pane);
        region.active = active;
        region.set_status({.path = active ? "active.cc" : "idle.cc",
                           .dirty = false,
                           .line = 1,
                           .column = 1,
                           .line_count = 1,
                           .revision = 0,
                           .style_origin = {},
                           .key = {}});
        return region;
    };
    Region active_status = status_region({2, 0, 1, 10}, "active/modeline", "window:0:1", true);
    Region inactive_status = status_region({5, 0, 1, 10}, "inactive/modeline", "window:1:1", false);
    Region echo{RegionRole::EchoArea, {6, 0, 1, scene.cols},  {},
                SurfaceClass::Echo,   VerticalAnchor::Bottom, "editor/echo"};
    echo.set_echo({.text = "window split below", .cursor_byte = std::nullopt});
    scene.regions = {std::move(active_status), std::move(inactive_status), std::move(echo)};

    const int width = presenter.cell_width() * scene.cols;
    const int height =
        presenter.cell_height() * 4 + presenter.cell_height() / 2 +
        static_cast<int>(presenter.status_bar_height() * 2.0F + presenter.echo_area_height());
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));
    const auto pixel = [&](int x, int y) {
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)];
    };
    const int workspace_bottom = height - static_cast<int>(presenter.echo_area_height());
    const int split_y = workspace_bottom / 2;
    const int sample_x = width - 2;
    CHECK(pixel(sample_x, split_y - 2) == theme.surface);
    CHECK(pixel(sample_x, workspace_bottom - 2) == theme.inactive_surface);

    const SkiaFrameLayout layout =
        presenter.prepare_layout(scene, static_cast<float>(width), static_cast<float>(height));
    const std::optional<ViewHit> hit = presenter.hit_test_view(
        layout, {.x = static_cast<float>(sample_x),
                 .y = static_cast<float>(workspace_bottom) - presenter.status_bar_height() * 0.5F});
    REQUIRE(hit);
    const std::optional<HitTarget> target = resolve_hit_target(scene, hit.value_or(ViewHit{}));
    REQUIRE(target);
    CHECK(target.value_or(HitTarget{}).pane_id == "window:1:1");
}

TEST_CASE("Skia partial rendering clears a cancelled cursor animation position") {
    SkiaPresenter presenter("monospace", 16.0F);
    SceneDamageTracker tracker;
    const auto make_scene = [](int cursor_row, int cursor_col) {
        Scene scene;
        scene.rows = 8;
        scene.cols = 20;
        scene.cursor_row = cursor_row;
        scene.cursor_col = cursor_col;
        Region body{RegionRole::TextArea, {0, 0, 6, 20}, {}};
        body.primitives().push_back({0, 0, "text", StyleClass::Text, false});
        Region status{
            RegionRole::StatusBar, {6, 0, 1, 20}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        Region echo{
            RegionRole::EchoArea, {7, 0, 1, 20}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    Scene scene = make_scene(4, 10);
    const int cell_width = presenter.cell_width();
    const int cell_height = presenter.cell_height();
    const int width = cell_width * scene.cols;
    const int height = cell_height * scene.rows;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> retained(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> reference(retained.size());

    REQUIRE(tracker.update(scene).full_repaint);
    const SkiaLogicalRect interpolated_cursor{
        .x = static_cast<float>(cell_width) / 2.0F,
        .y = static_cast<float>(cell_height) / 2.0F,
        .width = 2.0F,
        .height = static_cast<float>(cell_height),
    };
    presenter.render_animated(scene,
                              {.scroll_layers = {},
                               .view = {.cursor_owner = SkiaCursorOwner::Document,
                                        .cursor_rect = interpolated_cursor,
                                        .active_line_y = std::nullopt}},
                              width, height, retained.data(), row_bytes);

    scene = make_scene(5, 11);
    const SceneDamage scene_damage = tracker.update(scene);
    REQUIRE_FALSE(scene_damage.full_repaint);
    std::vector<SkiaLogicalRect> damage = presenter.damage_rects(
        scene, scene_damage, static_cast<float>(width), static_cast<float>(height));
    const SkiaLogicalRect target_cursor{
        .x = static_cast<float>((scene.cursor_col - 1) * cell_width),
        .y = static_cast<float>((scene.cursor_row - 1) * cell_height),
        .width = 2.0F,
        .height = static_cast<float>(cell_height),
    };
    std::vector<std::uint32_t> incomplete = retained;
    presenter.render_damage(scene, width, height, incomplete.data(), row_bytes, damage);
    presenter.render(scene, width, height, reference.data(), row_bytes);
    CHECK(incomplete != reference);

    append_cursor_transition_damage(damage, interpolated_cursor, target_cursor);
    presenter.render_damage(scene, width, height, retained.data(), row_bytes, damage);
    CHECK(retained == reference);
}
