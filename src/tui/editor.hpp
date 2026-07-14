#pragma once

#include <string>

namespace cind::tui {

// Terminal editor over the indent kernel (editor shell, phase A):
// every keystroke runs the real pipelines — type_char (with on-typing
// reindent), press_enter, Tab = reindent line, undo/redo on the undo tree.
// Highlighting is layer 1 of the editor plan: synchronous lexer tokens.
//
// Returns the process exit code. The style is discovered CLion-style: the
// nearest .clang-format above the file (LLVM fallback otherwise).
int run_editor(const std::string& path);

} // namespace cind::tui
