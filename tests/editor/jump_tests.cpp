#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/jump.hpp"

#include <cstdint>
#include <limits>
#include <string>

using namespace cind;

namespace {

JumpPosition position(BufferId buffer, AnchorId anchor, std::uint32_t line,
                      std::string resource = {}) {
    return {.buffer = buffer,
            .resource = std::move(resource),
            .anchor = anchor,
            .fallback = {.line = line, .byte_column = 0},
            .excerpt = "line"};
}

} // namespace

TEST_CASE("jump graph merges same-line positions and preserves branches") {
    JumpGraph graph;
    const BufferId source{0, 1};
    const BufferId first{1, 1};
    const BufferId second{2, 1};
    const JumpInternResult root = graph.intern(position(source, 1, 4, "/source.cc"));
    const JumpInternResult merged = graph.intern(position(source, 2, 4, "/source.cc"));
    const JumpInternResult left = graph.intern(position(first, 3, 8, "/left.cc"));
    const JumpInternResult right = graph.intern(position(second, 4, 12, "/right.cc"));

    CHECK(root.node == merged.node);
    CHECK(root.retained_position);
    CHECK_FALSE(merged.retained_position);
    REQUIRE(graph.link(root.node, left.node, "def"));
    REQUIRE(graph.link(root.node, right.node, "def"));
    REQUIRE(graph.outgoing(root.node).size() == 2);
    CHECK(graph.outgoing(root.node)[0].to == right.node);
    CHECK(graph.outgoing(root.node)[1].to == left.node);
}

TEST_CASE("jump graph detaches closed buffers without losing their resource position") {
    JumpGraph graph;
    const BufferId buffer{0, 1};
    const JumpInternResult node = graph.intern(position(buffer, 9, 7, "/closed.cc"));
    AnchorId removed = 0;
    graph.detach_buffer(
        buffer, [](AnchorId) { return LinePosition{.line = 11, .byte_column = 3}; },
        [&](AnchorId anchor) { removed = anchor; });

    const JumpNode* detached = graph.find(node.node);
    REQUIRE(detached != nullptr);
    CHECK_FALSE(detached->position.buffer.valid());
    CHECK(detached->position.anchor == 0);
    CHECK(detached->position.resource == "/closed.cc");
    CHECK(detached->position.fallback == LinePosition{.line = 11, .byte_column = 3});
    CHECK(removed == 9);
}

TEST_CASE("jump walk is append-only and bounds every movement") {
    JumpWalk walk;
    REQUIRE(walk.record(1));
    REQUIRE(walk.record(2));
    REQUIRE(walk.record(3));
    REQUIRE(walk.move(-2) == std::optional<JumpNodeId>{1});
    REQUIRE(walk.record(4));
    CHECK(walk.entries().size() == 4);
    CHECK(walk.entries()[1] == 2);
    CHECK(walk.entries()[2] == 3);
    CHECK(walk.current() == std::optional<JumpNodeId>{4});
    CHECK_FALSE(walk.move(std::numeric_limits<std::int64_t>::min()).has_value());
    CHECK_FALSE(walk.move(1).has_value());
}

TEST_CASE("jump graph eviction preserves persistent manual edges") {
    JumpGraph graph;
    const BufferId buffer{0, 1};
    const JumpNodeId first = graph.intern(position(buffer, 1, 1, "/source.cc")).node;
    const JumpNodeId manual = graph.intern(position(buffer, 2, 2, "/source.cc")).node;
    const JumpNodeId target = graph.intern(position(buffer, 3, 3, "/source.cc")).node;
    const JumpNodeId newest = graph.intern(position(buffer, 4, 4, "/source.cc")).node;
    REQUIRE(graph.link(manual, target, "manual", true));

    const std::vector<JumpNode> removed = graph.evict(2);

    REQUIRE(removed.size() == 2);
    CHECK(removed[0].id == first);
    CHECK(removed[1].id == newest);
    CHECK(graph.find(manual) != nullptr);
    CHECK(graph.find(target) != nullptr);
    CHECK(graph.edges().size() == 1);
}

TEST_CASE("jump walk removes evicted entries without losing its logical cursor") {
    JumpWalk walk;
    REQUIRE(walk.record(1));
    REQUIRE(walk.record(2));
    REQUIRE(walk.record(3));
    REQUIRE(walk.record(4));
    REQUIRE(walk.move(-1) == std::optional<JumpNodeId>{3});

    const std::vector<JumpNodeId> removed{2, 3};
    walk.forget(removed);

    REQUIRE(walk.entries().size() == 2);
    CHECK(walk.entries()[0] == 1);
    CHECK(walk.entries()[1] == 4);
    CHECK(walk.current() == std::optional<JumpNodeId>{1});
    CHECK(walk.move(1) == std::optional<JumpNodeId>{4});
}
