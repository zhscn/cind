#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "gui/skia_presenter.hpp"

#include <fontconfig/fontconfig.h>
#include <skia/core/SkGraphics.h>

#include <cmath>
#include <cstdint>
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
    body.prims.push_back({0, 2, " ", StyleClass::Text, true});
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

    CHECK(pixel(mid_x, mid_y) == theme.background);
    CHECK(pixel(presenter.cell_width() * 2 + mid_x, mid_y) == theme.selection_background);
    CHECK(pixel(mid_x, presenter.cell_height() + mid_y) == theme.status_background);
    CHECK(pixel(presenter.cell_width() * 6, mid_y) == theme.cursor);

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
                           static_cast<float>(mid_y)) == theme.selection_background);
        CHECK(scaled_pixel(static_cast<float>(presenter.cell_width() * 6),
                           static_cast<float>(mid_y)) == theme.cursor);
    }
}

TEST_CASE("Skia presenter paints semantic change-sign colors") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);

    Scene scene;
    scene.rows = 3;
    scene.cols = 2;
    Region signs{RegionRole::ChangeSigns, {0, 0, 3, 1}, {}, SurfaceClass::Gutter};
    signs.prims.push_back({0, 0, " ", StyleClass::SignAdded, false, PrimKind::ChangeBar});
    signs.prims.push_back({1, 0, " ", StyleClass::SignModified, false, PrimKind::ChangeBar});
    signs.prims.push_back({2, 0, " ", StyleClass::SignDeleted, false, PrimKind::ChangeDeletion});
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
    numbers.prims.push_back({0, 0, "1", StyleClass::Gutter, false});
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
            if (pixels[static_cast<std::size_t>(y * width + x)] != theme.gutter_background) {
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
    CHECK(primitive.cell_bounds.y == doctest::Approx(0.0F));
    CHECK(primitive.cell_bounds.height == doctest::Approx(presenter.cell_height()));
    CHECK(primitive.paint_bounds->y >= primitive.cell_bounds.y);
    CHECK(primitive.paint_bounds->y + primitive.paint_bounds->height <=
          primitive.cell_bounds.y + primitive.cell_bounds.height);
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
    status.prims.push_back({0, 0, "ok", StyleClass::StatusBar, false});
    scene.regions = {body, status, echo};

    const int width = presenter.cell_width() * scene.cols;
    const int cell_height = presenter.cell_height();
    const int partial_text_height = cell_height / 2;
    const int height = partial_text_height + 2 * cell_height;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height));
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));

    const auto pixel = [&](int y) {
        return pixels[static_cast<std::size_t>(y * width + width - 1)];
    };
    CHECK(pixel(partial_text_height - 1) == theme.background);
    CHECK(pixel(partial_text_height) == theme.status_background);
    CHECK(pixel(partial_text_height + cell_height - 1) == theme.status_background);
    CHECK(pixel(partial_text_height + cell_height) == theme.echo_background);
    CHECK(pixel(height - 1) == theme.echo_background);

    SceneDamageTracker tracker;
    REQUIRE(tracker.update(scene).full_repaint);
    scene.regions[1].prims.front().text = "ox";
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
        body.prims.push_back({0, 0, std::move(text), StyleClass::Text, false});
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
    CHECK(rectangles.front().width < logical_width);

    presenter.render_damage(scene, width, height, retained.data(), row_bytes, rectangles, scale);
    presenter.render(scene, width, height, reference.data(), row_bytes, scale);
    CHECK(retained == reference);

    scene = make_scene("abcxef", 18);
    const SceneDamage cursor_damage = tracker.update(scene);
    REQUIRE_FALSE(cursor_damage.full_repaint);
    CHECK(cursor_damage.cell_rects.empty());
    const std::vector<SkiaLogicalRect> cursor_rectangles =
        presenter.damage_rects(scene, cursor_damage, logical_width, logical_height);
    REQUIRE(cursor_rectangles.size() == 2);
    presenter.render_damage(scene, width, height, retained.data(), row_bytes, cursor_rectangles,
                            scale);
    presenter.render(scene, width, height, reference.data(), row_bytes, scale);
    CHECK(retained == reference);
}

TEST_CASE("Skia animation frames scroll only the grid and interpolate the cursor") {
    SkiaTheme theme;
    SkiaPresenter presenter("monospace", 16.0F, theme);
    const auto make_scene = [](std::string first, std::string second, int cursor_row) {
        Scene scene;
        scene.rows = 4;
        scene.cols = 12;
        scene.cursor_row = cursor_row;
        scene.cursor_col = 1;
        Region body{RegionRole::TextArea, {0, 0, 2, 12}, {}};
        body.prims.push_back({0, 0, std::move(first), StyleClass::Text, false});
        body.prims.push_back({1, 0, std::move(second), StyleClass::Text, false});
        Region status{
            RegionRole::StatusBar, {2, 0, 1, 12}, {}, SurfaceClass::Status, VerticalAnchor::Bottom};
        Region echo{
            RegionRole::EchoArea, {3, 0, 1, 12}, {}, SurfaceClass::Echo, VerticalAnchor::Bottom};
        scene.regions = {std::move(body), std::move(status), std::move(echo)};
        return scene;
    };

    const Scene source = make_scene("one", "two", 2);
    const Scene target = make_scene("two", "three", 1);
    const int cell_height = presenter.cell_height();
    const int width = presenter.cell_width() * target.cols;
    const int height = cell_height * target.rows;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint32_t);
    std::vector<std::uint32_t> source_pixels(static_cast<std::size_t>(width * height));
    std::vector<std::uint32_t> target_pixels(source_pixels.size());
    std::vector<std::uint32_t> animated_pixels(source_pixels.size());
    presenter.render(source, width, height, source_pixels.data(), row_bytes);
    presenter.render(target, width, height, target_pixels.data(), row_bytes);

    SkiaAnimationFrame start{
        .scroll_source = &source,
        .source_grid_offset_y = 0.0F,
        .target_grid_offset_y = static_cast<float>(cell_height),
        .cursor_position = std::nullopt,
    };
    presenter.render_animated(target, start, width, height, animated_pixels.data(), row_bytes);
    CHECK(animated_pixels == source_pixels);

    SkiaAnimationFrame finish{
        .scroll_source = &source,
        .source_grid_offset_y = -static_cast<float>(cell_height),
        .target_grid_offset_y = 0.0F,
        .cursor_position = std::nullopt,
    };
    presenter.render_animated(target, finish, width, height, animated_pixels.data(), row_bytes);
    CHECK(animated_pixels == target_pixels);

    SkiaAnimationFrame middle{
        .scroll_source = &source,
        .source_grid_offset_y = -static_cast<float>(cell_height) / 2.0F,
        .target_grid_offset_y = static_cast<float>(cell_height) / 2.0F,
        .cursor_position = SkiaLogicalPoint{.x = static_cast<float>(presenter.cell_width()) / 2.0F,
                                            .y = static_cast<float>(cell_height) / 2.0F},
    };
    presenter.render_animated(target, middle, width, height, animated_pixels.data(), row_bytes);
    const auto pixel = [&](int x, int y) {
        return animated_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x)];
    };
    CHECK(pixel(presenter.cell_width() / 2, cell_height / 2) == theme.cursor);
    CHECK(pixel(width - 1, cell_height * 2 + cell_height / 2) == theme.status_background);
    CHECK(pixel(width - 1, cell_height * 3 + cell_height / 2) == theme.echo_background);
}
