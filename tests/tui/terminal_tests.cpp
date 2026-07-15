#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tui/terminal.hpp"

using namespace cind::tui;

TEST_CASE("OSC 52 clipboard payload is base64 encoded") {
    CHECK(osc52_copy_sequence("foo") == "\x1b]52;c;Zm9v\a");
    CHECK(osc52_copy_sequence("f") == "\x1b]52;c;Zg==\a");
    CHECK(osc52_copy_sequence("fo") == "\x1b]52;c;Zm8=\a");
    CHECK(osc52_copy_sequence("\x1b]52;c;unsafe\a") == "\x1b]52;c;G101MjtjO3Vuc2FmZQc=\a");
}
