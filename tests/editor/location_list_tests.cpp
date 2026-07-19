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

TEST_CASE("location lists are independent workbench values with an explicit current list") {
    LocationListStack stack;
    const BufferId first_buffer{0, 1};
    const BufferId second_buffer{1, 1};
    const LocationListId first = stack.publish("search", {item("/a.cc", 1)}, first_buffer);
    const LocationListId second =
        stack.publish("diagnostic", {item("/b.cc", 2), item("/c.cc", 3)}, second_buffer);

    REQUIRE(stack.current() != nullptr);
    CHECK(stack.current()->id == second);
    CHECK(stack.current()->items.size() == 2);
    REQUIRE(stack.move(-1));
    CHECK(stack.current()->id == first);
    REQUIRE(stack.set_current_by_buffer(second_buffer));
    REQUIRE(stack.select(1));
    CHECK(stack.current()->selected == 1);
}

TEST_CASE("location resolution is lazy and detaches back to a durable range") {
    LocationListStack stack;
    const BufferId list_buffer{0, 1};
    const BufferId source_buffer{1, 1};
    (void)stack.publish("search", {item("/source.cc", 4)}, list_buffer);
    stack.resolve_resource("/source.cc", [&](const LocationItem&) {
        return ResolvedLocation{.buffer = source_buffer, .start = 10, .end = 11, .stale = false};
    });

    REQUIRE(stack.current()->items[0].resolved.has_value());
    std::vector<AnchorId> removed;
    stack.detach_buffer(
        source_buffer,
        [](AnchorId anchor) {
            return LinePosition{.line = static_cast<std::uint32_t>(anchor), .byte_column = 0};
        },
        [&](AnchorId anchor) { removed.push_back(anchor); });
    CHECK_FALSE(stack.current()->items[0].resolved.has_value());
    CHECK(stack.current()->items[0].range.start.line == 10);
    CHECK(stack.current()->items[0].range.end.line == 11);
    CHECK(removed == std::vector<AnchorId>{10, 11});
}
