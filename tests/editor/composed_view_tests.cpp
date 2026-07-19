#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/composed_view.hpp"
#include "editor/runtime.hpp"
#include "editor/transaction_group.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace cind;

TEST_CASE("composed views merge excerpts and pass edits through to source buffers") {
    EditorRuntime runtime;
    const BufferId first = runtime.buffers().create({.name = "first.cc",
                                                     .initial_text = "alpha\nbeta\n",
                                                     .kind = BufferKind::File,
                                                     .resource_uri = "/work/first.cc",
                                                     .read_only = false});
    const BufferId second = runtime.buffers().create({.name = "second.cc",
                                                      .initial_text = "gamma\ndelta\n",
                                                      .kind = BufferKind::File,
                                                      .resource_uri = "/work/second.cc",
                                                      .read_only = false});
    ComposedViewModel composed(
        runtime.buffers(),
        {{.buffer = first, .context = make_range(0, 6), .primary = make_range(0, 5)},
         {.buffer = first, .context = make_range(6, 11), .primary = make_range(6, 10)},
         {.buffer = second, .context = make_range(0, 6), .primary = make_range(0, 5)}});

    CHECK(composed.excerpts().size() == 2);
    const ComposedSnapshot snapshot = composed.snapshot();
    REQUIRE(snapshot.segments.size() == 2);
    const TextOffset first_edit{snapshot.segments[0].projection.start.value};
    const TextOffset second_edit{snapshot.segments[1].projection.start.value};
    const std::vector<TextEdit> edits{
        {.old_range = {.start = first_edit, .end = TextOffset{first_edit.value + 5}},
         .new_text = "ALPHA"},
        {.old_range = {.start = second_edit, .end = TextOffset{second_edit.value + 5}},
         .new_text = "GAMMA"}};
    const auto group_entries = composed.apply_edits(edits);

    REQUIRE(group_entries.has_value());
    CHECK(group_entries->size() == 2);
    CHECK(runtime.buffers().get(first).snapshot().content().to_string() == "ALPHA\nbeta\n");
    CHECK(runtime.buffers().get(second).snapshot().content().to_string() == "GAMMA\ndelta\n");

    TransactionGroupRegistry groups;
    const TransactionGroupId group = groups.record("composed-edit", *group_entries);
    const auto current = [&](BufferId buffer) -> std::optional<UndoNodeId> {
        return runtime.buffers().get(buffer).undo_position();
    };
    const auto navigate = [&](BufferId buffer, UndoNodeId position) {
        (void)runtime.buffers().get(buffer).undo_to(position);
        return true;
    };
    const std::optional<TransactionGroupResult> undone = groups.undo(group, current, navigate);
    REQUIRE(undone.has_value());
    CHECK(undone->changed.size() == 2);
    CHECK(runtime.buffers().get(first).snapshot().content().to_string() == "alpha\nbeta\n");
    CHECK(runtime.buffers().get(second).snapshot().content().to_string() == "gamma\ndelta\n");
    const std::optional<TransactionGroupResult> redone = groups.redo(group, current, navigate);
    REQUIRE(redone.has_value());
    CHECK(redone->changed.size() == 2);
}

TEST_CASE("transaction groups skip members that moved past the recorded snapshot") {
    TransactionGroupRegistry groups;
    const BufferId first{0, 1};
    const BufferId second{1, 1};
    const TransactionGroupId group =
        groups.record("rename", {{.buffer = first, .before = 1, .after = 2},
                                 {.buffer = second, .before = 4, .after = 5}});
    const auto current = [&](BufferId buffer) -> std::optional<UndoNodeId> {
        return buffer == first ? std::optional<UndoNodeId>{3} : std::optional<UndoNodeId>{5};
    };
    const auto navigate = [](BufferId, UndoNodeId) { return true; };

    const std::optional<TransactionGroupResult> result = groups.undo(group, current, navigate);
    REQUIRE(result.has_value());
    CHECK(result->changed == std::vector<BufferId>{second});
    CHECK(result->skipped == std::vector<BufferId>{first});
}
