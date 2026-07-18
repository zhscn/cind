#pragma once

#include "editor/runtime.hpp"

namespace cind {

struct SchemeMechanismsRegistration {
    LanguageProviderId structural_motion;
};

// Registers the native Scheme datum-navigation mechanism. Scheme policy owns
// the language profile and chooses which modes consume this capability.
SchemeMechanismsRegistration ensure_scheme_mechanisms(EditorRuntime& runtime);

} // namespace cind
