#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "editor/location_list.hpp"

#include <string>
#include <vector>

using namespace cind;

namespace {

LocationItem item(std::string resource, std::uint32_t line) {
    return {.resource = std::move(resource),
            .range = {.start = {.line = line, .column = 2, .encoding = PositionEncoding::Bytes},
                      .end = {.line = line, .column = 2, .encoding = PositionEncoding::Bytes}},
            .excerpt = "needle",
            .metadata = {{"severity", "error"}},
            .resolved = std::nullopt};
}

} // namespace

TEST_CASE("location list storage preserves independent published values") {
    LocationListStack stack;
    const BufferId first_buffer{0, 1};
    const BufferId second_buffer{1, 1};
    const LocationListId first = stack.publish("search", {item("/a.cc", 1)}, first_buffer);
    const LocationListId second =
        stack.publish("diagnostic", {item("/b.cc", 2), item("/c.cc", 3)}, second_buffer);

    REQUIRE(stack.find(first) != nullptr);
    CHECK(stack.find(first)->materialized_buffer == first_buffer);
    CHECK(stack.find(first)->items.size() == 1);
    REQUIRE(stack.find(second) != nullptr);
    CHECK(stack.find(second)->materialized_buffer == second_buffer);
    CHECK(stack.find(second)->items.size() == 2);
    CHECK(stack.find_by_buffer(second_buffer)->id == second);
}

TEST_CASE("location resolution is lazy and detaches back to a durable range") {
    LocationListStack stack;
    const BufferId list_buffer{0, 1};
    const BufferId source_buffer{1, 1};
    const LocationListId list = stack.publish("search", {item("/source.cc", 4)}, list_buffer);
    stack.resolve_resource("/source.cc", [&](const LocationItem&) {
        return ResolvedLocation{.buffer = source_buffer, .start = 10, .end = 11, .stale = false};
    });

    REQUIRE(stack.find(list)->items[0].resolved.has_value());
    std::vector<AnchorId> removed;
    stack.detach_buffer(
        source_buffer,
        [](AnchorId anchor) {
            return LinePosition{.line = static_cast<std::uint32_t>(anchor), .byte_column = 0};
        },
        [&](AnchorId anchor) { removed.push_back(anchor); });
    CHECK_FALSE(stack.find(list)->items[0].resolved.has_value());
    CHECK(stack.find(list)->items[0].range.start.line == 10);
    CHECK(stack.find(list)->items[0].range.end.line == 11);
    CHECK(removed == std::vector<AnchorId>{10, 11});
}
