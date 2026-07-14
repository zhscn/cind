#pragma once

#include "formatting/cpp_indent_style.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cind {

struct ClangFormatStyle {
    CppIndentStyle style;
    // The file said `BasedOnStyle: InheritParentConfig`: the caller should
    // resolve the next config file up the directory tree and re-parse this
    // file with that as `base`.
    bool inherit_parent = false;
    // `DisableFormat: true` — clang-format refuses to touch these files
    // (vendored code); they are not format ground truth either.
    bool disable_format = false;
    // Indentation-relevant keys we recognize but cannot honor, plus malformed
    // values. Everything else (line breaking, alignment cosmetics) is ignored
    // silently — it is the formatter's business, not the indent kernel's.
    std::vector<std::string> warnings;
};

// Reset `style` to one of clang-format's built-in presets (LLVM, Google,
// Chromium, Mozilla, WebKit, GNU, Microsoft — name is case-insensitive).
// Returns false and leaves `style` untouched for unknown names.
bool apply_clang_format_preset(std::string_view name, CppIndentStyle& style);

// Parse .clang-format YAML (top-level scalar subset) on top of `base`.
// Multi-document files apply every document whose `Language` is absent or
// `Cpp`; nested blocks are skipped. Never fails: unparsable content degrades
// to warnings.
ClangFormatStyle parse_clang_format_yaml(std::string_view yaml, const CppIndentStyle& base);

} // namespace cind
