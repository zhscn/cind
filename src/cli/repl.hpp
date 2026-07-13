#pragma once

#include <istream>
#include <string>

namespace cind {

// stdin command loop. `initial` may contain a '^' caret marker; without one
// the caret starts at the end of the text.
int run_repl(std::istream& in, std::string initial);

} // namespace cind
