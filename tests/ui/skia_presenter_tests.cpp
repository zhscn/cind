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
    presenter.render(scene, width, height, pixels.data(),
                     static_cast<std::size_t>(width) * sizeof(std::uint32_t));

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
}
