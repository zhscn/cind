#include "editor/cpp_mode.hpp"

#include <stdexcept>
#include <string>

namespace cind {

CppModeRegistration ensure_cpp_mode(EditorRuntime& runtime) {
    if (const std::optional<ModeId> existing = runtime.modes().find("cind.cpp")) {
        const std::optional<SettingId> dialect =
            runtime.setting_definitions().find("language.c-family.dialect");
        const std::optional<LanguageProfileId> language =
            runtime.languages().find_profile("cind.cpp");
        if (!dialect || !language) {
            throw std::logic_error("incomplete built-in C++ mode registration");
        }
        return {*dialect, *language, *existing};
    }

    const SettingId dialect = runtime.setting_definitions().define(
        "language.c-family.dialect", std::string("c++"),
        SettingScope::Language | SettingScope::Project | SettingScope::Buffer);
    const LanguageProviderId lexer =
        runtime.languages().define_provider("cind.c-family.lexer", LanguageFacet::Lexing);
    const LanguageProviderId syntax =
        runtime.languages().define_provider("cind.c-family.syntax", LanguageFacet::Syntax);
    const LanguageProviderId indentation = runtime.languages().define_provider(
        "cind.c-family.indentation", LanguageFacet::Indentation);
    const LanguageProviderId structural_editing = runtime.languages().define_provider(
        "cind.c-family.structural-editing", LanguageFacet::StructuralEditing);
    const LanguageProviderId highlighting = runtime.languages().define_provider(
        "cind.c-family.highlighting", LanguageFacet::Highlighting);

    const LanguageProfileId language = runtime.languages().define_profile("cind.cpp");
    runtime.languages().bind(language, LanguageFacet::Lexing, lexer);
    runtime.languages().bind(language, LanguageFacet::Syntax, syntax);
    runtime.languages().bind(language, LanguageFacet::Indentation, indentation);
    runtime.languages().bind(language, LanguageFacet::StructuralEditing, structural_editing);
    runtime.languages().bind(language, LanguageFacet::Highlighting, highlighting);
    runtime.languages().profile_for_configuration(language).defaults.set(dialect,
                                                                         std::string("c++"));
    const ModeId mode = runtime.modes().define("cind.cpp", ModeKind::Major, language);
    if (const std::optional<ModeId> parent = runtime.modes().find("prog-mode")) {
        runtime.modes().set_parent(mode, parent);
    } else {
        runtime.modes().set_interaction_class(mode, InteractionClass::Editing);
    }
    runtime.modes().set_things(
        mode, {{.name = "defun", .kind = "cst"}, {.name = "string", .kind = "cst"}});
    return {dialect, language, mode};
}

} // namespace cind
