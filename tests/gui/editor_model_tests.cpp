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

void send_keys(EditorModel& model, std::string_view notation) {
    const std::expected<KeySequence, KeyParseError> sequence = parse_key_sequence(notation);
    REQUIRE(sequence.has_value());
    for (const KeyStroke key : *sequence) {
        CHECK(model.handle_key(key, 10));
    }
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
    EditorModel model("sample.cc", source, 1);
    compose_frame(model, 8, 80);
    const TextOffset caret = model.inspect().caret;

    model.scroll_lines(10);
    const ui::Scene scrolled = compose_frame(model, 8, 80);
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.caret == caret);
    CHECK(state.viewport.top_line == 10);
    CHECK(state.viewport.top_line_offset == doctest::Approx(0.0F));
    CHECK(state.scripting.engine == "guile");
    CHECK(state.scripting.modules ==
          std::vector<std::string>{
              "cind application", "cind command",    "cind input",      "cind lsp",
              "cind async",       "cind lifecycle",  "cind pointer",    "cind extension",
              "cind emacs",       "cind toy-modal",  "cind meow",       "cind vim",
              "cind helix",       "cind structural", "cind paredit",    "cind minibuffer",
              "cind development", "cind ares",       "cind introspect", "cind core"});
    CHECK(state.scripting.command_revision == 1);
    CHECK(state.scripting.scripted_commands == 233);
    CHECK(state.scripting.provider_revision == 1);
    CHECK(state.scripting.scripted_providers == 16);
    CHECK(state.scripting.binding_revision == 1);
    CHECK(state.scripting.input_state_revision == 1);
    CHECK(state.scripting.scripted_input_states == 17);
    CHECK(state.scripting.scripted_input_strategies == 5);
    CHECK(state.scripting.mode_revision == 1);
    CHECK(state.scripting.scripted_modes == 8);
    CHECK(state.scripting.resource_policy_revision == 1);
    CHECK(state.scripting.scripted_file_mode_rules == 2);
    CHECK(state.scripting.scripted_project_providers == 3);
    REQUIRE(state.jumps.size() == 1);
    CHECK(state.jumps[0].nodes.empty());
    REQUIRE(state.jumps[0].walks.size() == 1);
    CHECK(state.jumps[0].walks[0].entries.empty());
    CHECK(state.text_input_policy == "accept");
    CHECK(state.text_input_command == "edit.self-insert");
    CHECK(state.text_input_command_available);
    CHECK(state.input_state == "emacs");
    CHECK(state.input_strategy == "emacs");
    CHECK(state.input_cursor_shape == "beam");
    CHECK(state.input_state_indicator.empty());
    REQUIRE(state.buffers.size() == 1);
    CHECK(state.buffers.front().interaction_class == "editing");
    CHECK(state.buffers.front().initial_input_state == "emacs");
    REQUIRE(state.windows.size() == 1);
    CHECK(state.windows.front().input_states == std::vector<std::string>{"emacs"});
    REQUIRE(state.workbenches.size() == 1);
    CHECK(state.workbenches.front().active);
    CHECK(state.workbenches.front().windows.size() == 1);
    CHECK(state.workbenches.front().layout.leaf);
    CHECK_FALSE(scrolled.cursor_visible);

    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 6));
    const ui::Scene revealed = compose_frame(model, 8, 80);
    CHECK(model.inspect().caret_position.line == 1);
    CHECK(revealed.cursor_visible);
}

TEST_CASE("inspection retains inactive workbench layouts and window state") {
    EditorModel model("sample.cc", "one\ntwo\n", 1);
    send_keys(model, "C-x 3");
    send_keys(model, "C-x w n");
    model.insert_text("notes");
    send_keys(model, "RET");

    const EditorStateSnapshot state = model.inspect();
    REQUIRE(state.workbenches.size() == 2);
    const auto original =
        std::ranges::find_if(state.workbenches, [](const WorkbenchStateSnapshot& workbench) {
            return workbench.name.empty();
        });
    const auto notes =
        std::ranges::find(state.workbenches, std::string{"notes"}, &WorkbenchStateSnapshot::name);
    REQUIRE(original != state.workbenches.end());
    REQUIRE(notes != state.workbenches.end());
    CHECK_FALSE(original->active);
    CHECK(original->windows.size() == 2);
    CHECK_FALSE(original->layout.leaf);
    CHECK(notes->active);
    CHECK(notes->windows.size() == 1);
}

TEST_CASE("fractional wheel scrolling preserves trackpad precision") {
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\nfour\n", 1);
    compose_frame(model, 4, 80);
    const TextOffset caret = model.inspect().caret;

    model.scroll_lines(0.25F);
    EditorStateSnapshot state = model.inspect();
    CHECK(state.caret == caret);
    CHECK(state.viewport.top_line == 0);
    CHECK(state.viewport.top_line_offset == doctest::Approx(0.25F));

    model.scroll_lines(1.5F);
    state = model.inspect();
    CHECK(state.viewport.top_line == 1);
    CHECK(state.viewport.top_line_offset == doctest::Approx(0.75F));
    const ui::Scene scene = compose_frame(model, 4, 80);
    CHECK(scene.grid_offset_rows == doctest::Approx(-0.75F));
    CHECK_FALSE(scene.cursor_visible);
}

TEST_CASE("discrete wheel scrolling uses scripted step policy") {
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\nfour\nfive\n", 1);
    compose_frame(model, 4, 80);

    model.scroll_steps(1.0F);

    const EditorStateSnapshot state = model.inspect();
    CHECK(state.viewport.top_line == 3);
    CHECK(state.viewport.top_line_offset == doctest::Approx(0.0F));
}

TEST_CASE("scripted toy normal state drives input, cursor, and modeline policy") {
    EditorModel model("sample.cc", "abc", 1);
    ui::Scene scene = compose_frame(model, 8, 80);
    CHECK(scene.cursor_shape == CursorShape::Beam);

    CHECK(model.handle_key(KeyStroke::character_key(U'c', KeyModifier::Control), 6));
    CHECK(model.handle_key(KeyStroke::character_key(U'n'), 6));
    EditorStateSnapshot state = model.inspect();
    CHECK(state.input_state == "toy-normal");
    CHECK(state.input_strategy == "toy-modal");
    CHECK(state.input_cursor_shape == "block");
    CHECK(state.input_state_indicator == "N");
    CHECK(state.text_input_policy == "ignore");

    scene = compose_frame(model, 8, 80);
    CHECK(scene.cursor_shape == CursorShape::Block);
    const ui::Region* status = scene.find(ui::RegionRole::StatusBar);
    REQUIRE(status != nullptr);
    REQUIRE(status->status() != nullptr);
    CHECK(std::ranges::any_of(status->status()->segments, [](const ModelineSegment& segment) {
        return segment.text == "N" && segment.group == ModelineGroup::Right;
    }));

    model.insert_text("z");
    CHECK(model.inspect().document_bytes == 3);
    CHECK(model.handle_key(KeyStroke::character_key(U'l'), 6));
    CHECK(model.inspect().caret.value == 1);
    CHECK(model.handle_key(KeyStroke::character_key(U'i'), 6));
    state = model.inspect();
    CHECK(state.input_state == "emacs");
    CHECK(state.input_strategy == "emacs");
    CHECK(state.text_input_policy == "accept");
    model.insert_text("z");
    CHECK(model.inspect().document_bytes == 4);
}

TEST_CASE("meow keypad feedback is exposed through the shared scene and inspector model") {
    EditorModel model("sample.cc", "text", 1);
    CHECK(model.handle_key(KeyStroke::character_key(U'c', KeyModifier::Control), 8));
    CHECK(model.handle_key(KeyStroke::character_key(U'm'), 8));
    CHECK(model.handle_key(KeyStroke::character_key(U'x'), 8));

    const EditorStateSnapshot state = model.inspect();
    CHECK(state.command_loop.pending_keys == "C-x");
    CHECK(state.command_loop.pending_keymap.empty());
    CHECK(state.command_loop.pending_input_state == "meow-keypad");
    CHECK(state.input_state_handler);
    CHECK_FALSE(state.input_state_on_enter);
    CHECK(state.input_state_on_exit);
    const ui::Scene scene = compose_frame(model, 40, 80);
    const ui::Region* popup = scene.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->popup() != nullptr);
    CHECK(popup->popup()->title == "C-x …");
    CHECK(std::ranges::any_of(popup->popup()->items, [](const ui::Region::PopupItem& item) {
        return item.label == "C-s" && item.detail == "file.save";
    }));
}

TEST_CASE("meow expansion hints share editor state Scene and inspector projections") {
    EditorModel model("sample.cc", "one two three four five six seven eight nine ten eleven twelve",
                      1);
    CHECK(model.handle_key(KeyStroke::character_key(U'c', KeyModifier::Control), 8));
    CHECK(model.handle_key(KeyStroke::character_key(U'm'), 8));
    CHECK(model.handle_key(KeyStroke::character_key(U'w'), 8));

    const EditorStateSnapshot state = model.inspect();
    CHECK(state.input_state == "meow-normal");
    CHECK(state.position_hints.provider);
    REQUIRE(state.position_hints.items.size() == 10);
    CHECK(state.position_hints.items.front().label == "1");
    CHECK(state.position_hints.items.back().label == "0");

    const ui::Scene scene = compose_frame(model, 8, 80);
    const ui::Region* body = scene.find(ui::RegionRole::TextArea);
    REQUIRE(body != nullptr);
    CHECK(std::ranges::count_if(body->primitives(), [](const ui::Prim& primitive) {
              return primitive.kind == ui::PrimKind::PositionHint;
          }) == 10);

    CHECK(model.handle_key(KeyStroke::character_key(U'x'), 8));
    const EditorStateSnapshot transient = model.inspect();
    CHECK(transient.input_state == "meow-keypad");
    CHECK_FALSE(transient.position_hints.provider);
    CHECK(transient.position_hints.items.empty());
}

TEST_CASE("caret reveal moves a fractional row to the top edge") {
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\nfour\n", 1);
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
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\n", 1);
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
    EditorModel model("sample.cc", "e\u0301x", 1);

    CHECK(model.handle_key(KeyStroke::named(KeyCode::Right), 10));
    CHECK(model.inspect().caret.value == 3);
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Backspace), 10));
    const EditorStateSnapshot state = model.inspect();
    CHECK(state.document_bytes == 1);
    CHECK(state.caret.value == 0);
}

TEST_CASE("vertical movement preserves the shared preferred display column") {
    EditorModel model("sample.cc", "abcd\nx\nabcd", 1);

    for (int index = 0; index < 3; ++index) {
        CHECK(model.handle_key(KeyStroke::named(KeyCode::Right), 10));
    }
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 10));
    CHECK(model.inspect().caret_display_column == 1);
    CHECK(model.handle_key(KeyStroke::named(KeyCode::Down), 10));
    CHECK(model.inspect().caret_display_column == 3);
}

TEST_CASE("search uses the shared non-blocking interaction state") {
    EditorModel model("sample.cc", "one two one", 1);

    CHECK(model.handle_key(KeyStroke::character_key(U's', KeyModifier::Control), 10));
    EditorStateSnapshot state = model.inspect();
    CHECK(state.interaction.active);
    CHECK(state.interaction.keymap == "interaction.text");
    CHECK(state.interaction.input_state == "emacs");
    CHECK(state.interaction.prompt == "search: ");
    CHECK(state.input_focus == "minibuffer");
    CHECK(state.interaction.origin_window_slot == state.active_window_slot);
    CHECK(state.interaction.origin_window_generation == state.active_window_generation);
    CHECK(state.interaction.window_slot != state.interaction.origin_window_slot);
    CHECK(state.interaction.buffer_slot != state.buffers.front().buffer_slot);
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
    EditorModel model("sample.cc", "text", 1);

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

TEST_CASE("picker input keeps one stable minibuffer region with no matches") {
    EditorModel model("sample.cc", "text", 1);

    REQUIRE(model.handle_key(KeyStroke::character_key(U'x', KeyModifier::Alt), 10));
    const ui::Scene initial = compose_frame(model, 20, 100);
    const ui::Region* initial_popup = initial.find(ui::RegionRole::Popup);
    REQUIRE(initial_popup != nullptr);
    const int popup_rows = initial_popup->rect.rows;

    model.insert_text("no-command-can-match-this-query");
    ui::Scene empty = compose_frame(model, 20, 100);
    const ui::Region* popup = empty.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    REQUIRE(popup->popup());
    CHECK(popup->rect.rows == popup_rows);
    CHECK(popup->popup()->items.empty());
    CHECK(popup->popup()->input == "no-command-can-match-this-query");
    const ui::Region* echo = empty.find(ui::RegionRole::EchoArea);
    REQUIRE(echo != nullptr);
    REQUIRE(echo->echo());
    CHECK(echo->echo()->text.empty());
    CHECK_FALSE(echo->echo()->cursor_byte);

    REQUIRE(model.handle_key(KeyStroke::named(KeyCode::Backspace), 10));
    empty = compose_frame(model, 20, 100);
    popup = empty.find(ui::RegionRole::Popup);
    REQUIRE(popup != nullptr);
    CHECK(popup->rect.rows == popup_rows);
}

TEST_CASE("semantic pointer targets edit document positions without viewport reconstruction") {
    EditorModel model("sample.cc", "zero\none\ntwo\n", 1);
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
    EditorModel model("sample.cc", "zero\none\ntwo\nthree\n", 1);

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

TEST_CASE("split completion belongs to the active pane and never covers its caret row") {
    EditorModel model("sample.scm", "", 1);

    send_keys(model, "C-c C-z");
    model.insert_text("(d");
    REQUIRE(model.inspect().completion.active);

    const ui::Scene scene = compose_frame(model, 40, 100);
    const auto completions = std::ranges::count_if(scene.regions, [](const ui::Region& region) {
        return region.role == ui::RegionRole::Popup && region.popup() != nullptr &&
               region.popup()->presentation == ui::Region::PopupPresentation::Completion;
    });
    CHECK(completions == 1);
    const auto completion = std::ranges::find_if(scene.regions, [](const ui::Region& region) {
        return region.role == ui::RegionRole::Popup && region.popup() != nullptr &&
               region.popup()->presentation == ui::Region::PopupPresentation::Completion;
    });
    REQUIRE(completion != scene.regions.end());
    CHECK_FALSE(completion->pane_id.empty());
    CHECK(completion->active);
    const int caret_row = scene.cursor_row - 1;
    CHECK((caret_row < completion->rect.row ||
           caret_row >= completion->rect.row + completion->rect.rows));
}

TEST_CASE("command palette retains a global selection and stable list viewport") {
    EditorModel model("sample.cc", "text", 1);

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
        EditorModel model(path.string(), "old", 1);
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
