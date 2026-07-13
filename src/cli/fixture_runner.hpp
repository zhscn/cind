#pragma once

namespace cind {

// Runs one fixture file or every *.yaml under a directory (recursively).
// Prints PASS/FAIL per fixture; returns a process exit code.
int run_fixtures(const char* path);

} // namespace cind
