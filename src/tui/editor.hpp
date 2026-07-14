#pragma once

#include <cstdint>
#include <string>

namespace cind::tui {

// Terminal editor over the indent kernel (editor shell, phase A):
// every keystroke runs the real pipelines — type_char (with on-typing
// reindent), press_enter, Tab = reindent line, undo/redo on the undo tree.
// Highlighting is layer 1 of the editor plan: synchronous lexer tokens.
//
// Returns the process exit code. The style is discovered CLion-style: the
// nearest .clang-format above the file (LLVM fallback otherwise).
// `initial_line` is 1-based (the CLI's +N argument); 0 means line 1.
int run_editor(const std::string& path, std::uint32_t initial_line = 0);

} // namespace cind::tui
