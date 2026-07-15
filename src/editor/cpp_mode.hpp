#pragma once

#include "editor/runtime.hpp"

namespace cind {

struct CppModeRegistration {
    SettingId dialect;
    LanguageProfileId language;
    ModeId mode;
};

// Registers the built-in C-family editing providers and the C++ profile in a
// runtime. The provider identities are declarative here; their native hot-path
// implementations remain in the lexer/syntax/indentation libraries.
CppModeRegistration ensure_cpp_mode(EditorRuntime& runtime);

} // namespace cind
