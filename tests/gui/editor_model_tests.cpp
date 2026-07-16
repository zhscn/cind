#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "gui/editor_model.hpp"
#include "gui/motion.hpp"
#include "gui/scroll_timeline.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

using namespace cind;
using namespace cind::gui;

namespace {

ui::Scene compose_frame(EditorModel& model, int rows, int columns, float visible_text_rows = 0.0F) {
    model.layout_view(rows, columns, visible_text_rows);
    return model.compose(rows, columns, visible_text_rows);
}

} // namespace

TEST_CASE("critical spring preserves motion when its target changes") {
    constexpr float frequency = 32.0F;
    const SpringState moving = advance_critical_spring(
        {.position = 0.0F, .velocity = 0.0F},
        {.target = 1.0F, .angular_frequency = frequency, .elapsed_seconds = 0.05F});
    REQUIRE(moving.position > 0.0F);
    REQUIRE(moving.velocity > 0.0F);

    const SpringState retargeted = advance_critical_spring(
        moving, {.target = 2.0F, .angular_frequency = frequency, .elapsed_seconds = 0.0F});
    CHECK(retargeted.position == doctest::Approx(moving.position));
    CHECK(retargeted.velocity == doctest::Approx(moving.velocity));
}

TEST_CASE("critical spring integration is stable across frame splits") {
    constexpr float frequency = 32.0F;
    const SpringState single = advance_critical_spring(
        {.position = 0.0F, .velocity = 0.0F},
        {.target = 1.0F, .angular_frequency = frequency, .elapsed_seconds = 0.1F});
    SpringState split = advance_critical_spring(
        {.position = 0.0F, .velocity = 0.0F},
        {.target = 1.0F, .angular_frequency = frequency, .elapsed_seconds = 0.04F});
    split = advance_critical_spring(
        split, {.target = 1.0F, .angular_frequency = frequency, .elapsed_seconds = 0.06F});

    CHECK(split.position == doctest::Approx(single.position).epsilon(0.0001));
    CHECK(split.velocity == doctest::Approx(single.velocity).epsilon(0.0001));
    const SpringState settled = advance_critical_spring(
        single, {.target = 1.0F, .angular_frequency = frequency, .elapsed_seconds = 0.4F});
    CHECK(spring_at_rest(
        settled, {.target = 1.0F, .position_tolerance = 0.001F, .velocity_tolerance = 0.01F}));
}

TEST_CASE("scroll timeline brackets a sustained retargeted motion") {
    ScrollSceneTimeline timeline;
    const auto scene = [](int marker) {
        ui::Scene result;
        result.rows = marker;
        return result;
    };

    timeline.insert(scene(0), 0.0F);
    for (int target = 1; target <= 500; ++target) {
        timeline.insert(scene(target), static_cast<float>(target));
        const float visual = std::max(0.0F, static_cast<float>(target) - 3.25F);
        timeline.retain_motion_range(visual, static_cast<float>(target));
    }

    const std::vector<ScrollSceneLayer> layers = timeline.layers_at(496.75F);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].scroll_top == doctest::Approx(496.0F));
    CHECK(layers[0].scene->rows == 496);
    CHECK(layers[1].scroll_top == doctest::Approx(497.0F));
    CHECK(layers[1].scene->rows == 497);
    CHECK(timeline.size() <= 6);
}

TEST_CASE("scroll timeline brackets the visual position after reversing direction") {
    ScrollSceneTimeline timeline;
    for (int target = 0; target <= 8; ++target) {
        ui::Scene scene;
        scene.rows = target;
        timeline.insert(std::move(scene), static_cast<float>(target));
    }
    timeline.retain_motion_range(5.4F, 3.0F);

    const std::vector<ScrollSceneLayer> layers = timeline.layers_at(5.4F);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].scroll_top == doctest::Approx(5.0F));
    CHECK(layers[1].scroll_top == doctest::Approx(6.0F));
}

TEST_CASE("scroll timeline preserves equivalent scene identity at one position") {
    ScrollSceneTimeline timeline;
    auto original = std::make_shared<const ui::Scene>();
    timeline.insert(original, 0.0F);
    timeline.insert(std::make_shared<const ui::Scene>(), 0.0F);

    const std::vector<ScrollSceneLayer> layers = timeline.layers_at(0.0F);
    REQUIRE(layers.size() == 1);
    CHECK(layers.front().scene == original);
}

TEST_CASE("wheel scrolling moves the viewport without moving the caret") {
    std::string source;
    for (int line = 0; line < 30; ++line) {
        source += "line\n";
    }
    EditorModel model("sample.cc", source, CppIndentStyle{}, "test", 1);
    compose_frame(model, 8, 80);
    const TextOffset caret = model.inspect().caret;

    model.scroll_lines(10);
    const ui::Scene scrolled = compose_frame(model, 8, 80);
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.caret == caret);
    CHECK(state.viewport.top_line == 10);
    CHECK(state.scripting.engine == "guile");
    CHECK(state.scripting.modules == std::vector<std::string>{"cind command", "cind core"});
    CHECK(state.scripting.command_revision == 1);
    CHECK(state.scripting.scripted_commands == 37);
    CHECK(state.scripting.provider_revision == 1);
    CHECK(state.scripting.scripted_providers == 4);
    CHECK(state.scripting.binding_revision == 1);
    CHECK(state.scripting.input_state_revision == 1);
    CHECK(state.scripting.scripted_input_states == 1);
    REQUIRE(state.windows.size() == 1);
    CHECK(state.windows.front().input_states == std::vector<std::string>{"emacs"});
    CHECK_FALSE(scrolled.cursor_visible);

    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 6));
    const ui::Scene revealed = compose_frame(model, 8, 80);
    CHECK(model.inspect().caret_position.line == 1);
    CHECK(revealed.cursor_visible);
}

TEST_CASE("caret reveal moves a fractional row to the top edge") {
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\nfour\n", CppIndentStyle{}, "test", 1);
    compose_frame(model, 6, 80, 3.5F);

    for (int line = 0; line < 3; ++line) {
        CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 3));
    }

    const ui::Scene scene = compose_frame(model, 6, 80, 3.5F);
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.caret_position.line == 3);
    CHECK(state.viewport.top_line == 0);
    CHECK(state.viewport.top_line_offset == doctest::Approx(0.5F));
    CHECK(scene.grid_offset_rows == doctest::Approx(-0.5F));
    CHECK(scene.cursor_visible);
    CHECK(scene.cursor_row == 4);
}

TEST_CASE("scene composition does not mutate retained view state") {
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\n", CppIndentStyle{}, "test", 1);
    for (int line = 0; line < 3; ++line) {
        CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 2));
    }
    model.layout_view(5, 40, 2.5F);
    const ui::EditorViewport before = model.inspect().viewport;

    const ui::Scene first = model.compose(5, 40, 2.5F);
    const ui::Scene second = model.compose(5, 40, 2.5F);
    const ui::EditorViewport after = model.inspect().viewport;

    CHECK(after.top_line == before.top_line);
    CHECK(after.top_line_offset == doctest::Approx(before.top_line_offset));
    CHECK(after.left_column == before.left_column);
    CHECK(second.grid_offset_rows == doctest::Approx(first.grid_offset_rows));
    CHECK(second.cursor_row == first.cursor_row);
}

TEST_CASE("cursor movement and deletion operate on extended grapheme clusters") {
    EditorModel model("sample.cc", "e\u0301x", CppIndentStyle{}, "test", 1);

    CHECK(model.handle_key(KeyStroke::named(KeyCode::Right), 10));
    CHECK(model.inspect().caret.value == 3);
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Backspace), 10));
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.document_bytes == 1);
    CHECK(state.caret.value == 0);
}

TEST_CASE("vertical movement preserves the shared preferred display column") {
    EditorModel model("sample.cc", "abcd\nx\nabcd", CppIndentStyle{}, "test", 1);

    for (int index = 0; index < 3; ++index) {
        CHECK(model.handle_key(KeyStroke::named(KeyCode::Right), 10));
    }
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 10));
    CHECK(model.inspect().caret_display_column == 1);
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 10));
    CHECK(model.inspect().caret_display_column == 3);
}

TEST_CASE("search uses the shared non-blocking interaction state") {
    EditorModel model("sample.cc", "one two one", CppIndentStyle{}, "test", 1);

    CHECK(model.handle_key(KeyStroke::character_key(U's', KeyModifier::Control), 10));
    EditorStateSnapshot state = model.inspect();
    CHECK(state.interaction.active);
    CHECK(state.interaction.prompt == "search: ");
    CHECK(state.command_loop.last_command == "search.prompt");
    ui::Scene scene = compose_frame(model, 8, 80);
    const ui::Region* echo = scene.find(ui::RegionRole::EchoArea);
    REQUIRE(echo != nullptr);
    REQUIRE(echo->echo());
    const ui::Region::EchoContent initial_echo = *echo->echo();
    CHECK(initial_echo.text == "search: ");
    CHECK(initial_echo.cursor_byte == 8);

    model.insert_text("two");
    scene = compose_frame(model, 8, 80);
    echo = scene.find(ui::RegionRole::EchoArea);
    REQUIRE(echo != nullptr);
    REQUIRE(echo->echo());
    const ui::Region::EchoContent edited_echo = *echo->echo();
    CHECK(edited_echo.text == "search: two");
    CHECK(edited_echo.cursor_byte == 11);
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Enter), 10));
    state = model.inspect();
    CHECK_FALSE(state.interaction.active);
    CHECK(state.command_loop.last_command == "search.accept");
    CHECK(state.caret.value == 4);
}

TEST_CASE("prefix help and picker candidates compose a semantic popup") {
    EditorModel model("sample.cc", "text", CppIndentStyle{}, "test", 1);

    CHECK(model.handle_key(KeyStroke::character_key(U'x', KeyModifier::Control), 10));
    ui::Scene scene = compose_frame(model, 16, 100);
    const ui::Region* popup = scene.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    CHECK(popup->vertical_anchor == ui::VerticalAnchor::Bottom);
    REQUIRE(popup->popup());
    CHECK_FALSE(popup->popup()->input);
    CHECK(std::ranges::any_of(popup->popup()->items, [](const ui::Region::PopupItem& item) {
        return item.label.find("C-s") != std::string::npos &&
               item.detail.find("file.save") != std::string::npos;
    }));

    CHECK(model.handle_key(KeyStroke::character_key(U'g', KeyModifier::Control), 10));
    CHECK(model.handle_key(KeyStroke::character_key(U'x', KeyModifier::Alt), 10));
    scene = compose_frame(model, 16, 100);
    popup = scene.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->popup());
    REQUIRE(popup->popup()->input);
    CHECK(popup->popup()->title == "Command: ");
    CHECK(popup->popup()->selected_item.has_value());

    model.insert_text("edit");
    REQUIRE(model.handle_key(KeyStroke::character_key(U'b', KeyModifier::Control), 10));
    scene = compose_frame(model, 16, 100);
    popup = scene.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->popup());
    CHECK(popup->popup()->input == "edit");
    CHECK(popup->popup()->input_cursor == 3);
}

TEST_CASE("semantic pointer targets edit document positions without viewport reconstruction") {
    EditorModel model("sample.cc", "zero\none\ntwo\n", CppIndentStyle{}, "test", 1);
    model.click(ui::HitTarget{.kind = ui::HitTargetKind::DocumentText,
                              .view_id = "editor/document",
                              .pane_id = {},
                              .region_index = 0,
                              .role = ui::RegionRole::TextArea,
                              .scene_cell = std::nullopt,
                              .local_cell = std::nullopt,
                              .document_line = 1,
                              .display_column = 2,
                              .popup_item = std::nullopt});
    EditorStateSnapshot state = model.inspect();
    CHECK(state.caret_position.line == 1);
    CHECK(state.caret_display_column == 2);

    model.click(ui::HitTarget{.kind = ui::HitTargetKind::DocumentGutter,
                              .view_id = "editor/gutter/line-numbers",
                              .pane_id = {},
                              .region_index = 1,
                              .role = ui::RegionRole::LineNumbers,
                              .scene_cell = std::nullopt,
                              .local_cell = std::nullopt,
                              .document_line = 2,
                              .display_column = std::nullopt,
                              .popup_item = std::nullopt});
    state = model.inspect();
    CHECK(state.caret_position.line == 2);
    CHECK(state.caret_display_column == 0);
}

TEST_CASE("window splits compose pane-owned regions with active theme state") {
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\n", CppIndentStyle{}, "test", 1);

    REQUIRE(model.handle_key(KeyStroke::character_key(U'x', KeyModifier::Control), 8));
    REQUIRE(model.handle_key(KeyStroke::character_key(U'3'), 8));
    ui::Scene scene = compose_frame(model, 12, 80);
    REQUIRE(scene.panes.size() == 2);
    REQUIRE(scene.dividers.size() == 1);
    CHECK(scene.dividers.front().axis == ui::DividerAxis::Vertical);
    CHECK(std::ranges::count_if(scene.panes,
                                [](const ui::ScenePane& pane) { return pane.active; }) == 1);
    CHECK(std::ranges::count_if(scene.regions, [](const ui::Region& region) {
              return region.role == ui::RegionRole::StatusBar && region.active;
          }) == 1);
    CHECK(std::ranges::count_if(scene.regions, [](const ui::Region& region) {
              return region.role == ui::RegionRole::StatusBar && !region.active;
          }) == 1);
    CHECK(std::ranges::all_of(scene.regions, [](const ui::Region& region) {
        return region.pane_id.empty() ||
               (region.role == ui::RegionRole::StatusBar
                    ? region.vertical_anchor == ui::VerticalAnchor::Cell
                    : region.vertical_anchor == ui::VerticalAnchor::PaneGrid);
    }));

    const std::string first_active =
        std::ranges::find_if(scene.panes, [](const ui::ScenePane& pane) {
            return pane.active;
        })->id;
    REQUIRE(model.handle_key(KeyStroke::character_key(U'x', KeyModifier::Control), 8));
    REQUIRE(model.handle_key(KeyStroke::character_key(U'o'), 8));
    scene = compose_frame(model, 12, 80);
    const std::string second_active =
        std::ranges::find_if(scene.panes, [](const ui::ScenePane& pane) {
            return pane.active;
        })->id;
    CHECK(second_active != first_active);

    const auto inactive_document =
        std::ranges::find_if(scene.regions, [](const ui::Region& region) {
            return region.role == ui::RegionRole::TextArea && !region.active;
        });
    REQUIRE(inactive_document != scene.regions.end());
    model.click({.kind = ui::HitTargetKind::DocumentText,
                 .view_id = inactive_document->id,
                 .pane_id = inactive_document->pane_id,
                 .region_index = static_cast<std::size_t>(
                     std::distance(scene.regions.begin(), inactive_document)),
                 .role = ui::RegionRole::TextArea,
                 .scene_cell = std::nullopt,
                 .local_cell = std::nullopt,
                 .document_line = 0,
                 .display_column = 0,
                 .popup_item = std::nullopt});
    scene = compose_frame(model, 12, 80);
    CHECK(std::ranges::find_if(scene.panes, [](const ui::ScenePane& pane) {
              return pane.active;
          })->id == first_active);
}

TEST_CASE("command palette retains a global selection and stable list viewport") {
    EditorModel model("sample.cc", "text", CppIndentStyle{}, "test", 1);

    REQUIRE(model.handle_key(KeyStroke::character_key(U'x', KeyModifier::Alt), 10));
    ui::Scene scene = compose_frame(model, 20, 100);
    const ui::Region* popup = scene.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->popup());
    const std::size_t capacity = popup->popup()->items.size();
    REQUIRE(capacity > 1);
    const std::size_t downward_steps = capacity + 4;
    REQUIRE(popup->popup()->total_items > downward_steps);
    CHECK(popup->popup()->selected_item == 0);

    for (std::size_t step = 0; step < downward_steps; ++step) {
        REQUIRE(model.handle_key(KeyStroke::character_key(U'n', KeyModifier::Control), 10));
        scene = compose_frame(model, 20, 100);
    }
    popup = scene.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->popup());
    CHECK(popup->popup()->selected_item == downward_steps);
    CHECK(popup->popup()->first_item == downward_steps - capacity + 1);
    CHECK(popup->popup()->selected_item == popup->popup()->first_item + capacity - 1);

    REQUIRE(model.handle_key(KeyStroke::character_key(U'p', KeyModifier::Control), 10));
    scene = compose_frame(model, 20, 100);
    popup = scene.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->popup());
    CHECK(popup->popup()->selected_item == downward_steps - 1);
    CHECK(popup->popup()->first_item == downward_steps - capacity + 1);
    CHECK(popup->popup()->selected_item == popup->popup()->first_item + capacity - 2);
}

TEST_CASE("background save captures one revision without blocking newer edits") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("cind-editor-save-{}.cc", static_cast<long>(::getpid()));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    {
        EditorModel model(path.string(), "old", CppIndentStyle{}, "test", 1);
        model.insert_text("x");
        CHECK(model.handle_key(KeyStroke::character_key(U'x', KeyModifier::Control), 10));
        CHECK(model.handle_key(KeyStroke::character_key(U's', KeyModifier::Control), 10));
        model.insert_text("y");

        for (int attempt = 0; attempt < 200 && model.has_background_work(); ++attempt) {
            model.poll_background_work();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE_FALSE(model.has_background_work());
        CHECK(model.inspect().dirty);
    }

    std::ifstream input(path, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    CHECK(saved == "xold");
    std::filesystem::remove(path, ignored);
}
