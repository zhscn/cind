#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "project/search_results.hpp"

using namespace cind;

TEST_CASE("ripgrep results preserve colon paths and create semantic locations") {
    const std::string output = std::string("./src/a:b.cpp\0", 14) + "12:7:needle: value\n" +
                               std::string("./src/other.cpp\0", 16) + "2:1:needle\n";
    const auto parsed =
        parse_rg_search_results({.project_root = "/work/project", .output = output});
    REQUIRE(parsed.has_value());
    CHECK(parsed->text == "src/a:b.cpp:12:7: needle: value\nsrc/other.cpp:2:1: needle\n");
    REQUIRE(parsed->locations.size() == 2);
    CHECK(parsed->locations[0].resource == "/work/project/src/a:b.cpp");
    CHECK(parsed->locations[0].target == LinePosition{.line = 11, .byte_column = 6});
    CHECK(parsed->locations[0].excerpt == "needle: value");
    CHECK(parsed->locations[1].source_range.start == parsed->locations[0].source_range.end);
}

TEST_CASE("ripgrep result parsing rejects ambiguous records") {
    const auto parsed =
        parse_rg_search_results({.project_root = "/work", .output = "file.cpp:1:2:text\n"});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().find("path separator") != std::string::npos);
}
