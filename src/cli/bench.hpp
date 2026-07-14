#pragma once

#include <string>
#include <vector>

namespace cind {

struct BenchOptions {
    std::string style_preset = "default"; // "default" (4-space) or "llvm" (2-space)
    int show_mismatches = 0;              // print up to N mismatching lines
    // Ground-truth hygiene: skip files whose current content differs from
    // clang-format's own output (legacy formatting is noise, not signal).
    bool clean_only = false;
};

// Measures per-line indentation accuracy against already-formatted sources:
// for every non-blank line, compare the engine's target column with the
// file's actual indentation and classify mismatches by FormatRole.
int run_bench(const std::vector<std::string>& paths, const BenchOptions& options);

} // namespace cind
