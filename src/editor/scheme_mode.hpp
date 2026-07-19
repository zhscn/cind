#pragma once

#include "editor/runtime.hpp"

namespace cind {

struct SchemeMechanismsRegistration {
    LanguageProviderId lexer;
    LanguageProviderId indentation;
    LanguageProviderId structural_motion;
    LanguageProviderId structural_editing;
    LanguageProviderId highlighting;
};

// Registers the native Scheme language mechanism. Scheme policy owns the
// language profile and chooses which modes consume its facets.
SchemeMechanismsRegistration ensure_scheme_mechanisms(EditorRuntime& runtime);

} // namespace cind
