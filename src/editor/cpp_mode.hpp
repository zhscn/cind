#pragma once

#include "editor/runtime.hpp"

namespace cind {

struct CFamilyMechanismsRegistration {
    SettingId dialect;
    LanguageProviderId lexer;
    LanguageProviderId syntax;
    LanguageProviderId indentation;
    LanguageProviderId structural_editing;
    LanguageProviderId highlighting;
};

// Registers the native C-family capability inventory. Profiles and modes are
// policy and are composed separately by the embedding layer.
CFamilyMechanismsRegistration ensure_c_family_mechanisms(EditorRuntime& runtime);

} // namespace cind
