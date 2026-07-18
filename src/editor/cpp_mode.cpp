#include "editor/cpp_mode.hpp"

#include "commands/editor_commands.hpp"
#include "editor/language_mechanism.hpp"
#include "syntax/structure.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace cind {

namespace {

class CFamilyMechanismSession final : public LanguageMechanismSession {
public:
    const Analysis& analysis(const DocumentSnapshot& snapshot) override {
        return analyzer_.analyze(snapshot);
    }

    void apply(const DocumentChange& change, const DocumentSnapshot& snapshot) override {
        analyzer_.apply(change, snapshot);
    }

    TypeCharsResult type_chars(Document& document, std::span<const TextOffset> carets,
                               char character, const CppIndentStyle& style) override {
        return cind::type_chars(document, carets, character, style, analyzer_);
    }

    EnterResult newline(Document& document, TextOffset caret,
                        const CppIndentStyle& style) override {
        return press_enter(document, caret, style, analyzer_);
    }

    IndentDecision indent_line(Document& document, std::uint32_t line,
                               const CppIndentStyle& style) override {
        return cind::indent_line(document, line, style, analyzer_);
    }

    IndentDecision explain_indent(const DocumentSnapshot& snapshot, std::uint32_t line,
                                  const CppIndentStyle& style) override {
        return compute_line_indent(snapshot, analyzer_.analyze(snapshot).tree, line, style);
    }

    std::optional<TextOffset> move_structurally(const DocumentSnapshot& snapshot, TextOffset from,
                                                StructuralMotion motion) override {
        const SyntaxTree& tree = analyzer_.analyze(snapshot).tree;
        switch (motion) {
        case StructuralMotion::ForwardExpression:
            if (const std::optional<TextRange> range = sexp_forward(tree, from)) {
                return range->end;
            }
            break;
        case StructuralMotion::BackwardExpression:
            if (const std::optional<TextRange> range = sexp_backward(tree, from)) {
                return range->start;
            }
            break;
        case StructuralMotion::UpList:
            if (const std::optional<TextRange> range = enclosing_list(tree, from)) {
                return range->start;
            }
            break;
        }
        return std::nullopt;
    }

private:
    Analyzer analyzer_;
};

std::shared_ptr<const LanguageMechanism> c_family_mechanism() {
    static const std::shared_ptr<const LanguageMechanism> mechanism =
        std::make_shared<const LanguageMechanism>(
            LanguageFacet::Lexing | LanguageFacet::Syntax | LanguageFacet::Indentation |
                LanguageFacet::StructuralMotion | LanguageFacet::StructuralEditing |
                LanguageFacet::Highlighting,
            [] { return std::make_unique<CFamilyMechanismSession>(); });
    return mechanism;
}

} // namespace

CFamilyMechanismsRegistration ensure_c_family_mechanisms(EditorRuntime& runtime) {
    const std::shared_ptr<const LanguageMechanism> mechanism = c_family_mechanism();
    const auto provider = [&](std::string name, LanguageFacet facet) {
        if (const std::optional<LanguageProviderId> existing =
                runtime.languages().find_provider(name)) {
            const LanguageRegistry::ProviderDefinition& definition =
                runtime.languages().provider(*existing);
            if (definition.facet != facet || definition.mechanism != mechanism) {
                throw std::logic_error("C-family provider has an incompatible mechanism");
            }
            return *existing;
        }
        return runtime.languages().define_provider(std::move(name), facet, mechanism);
    };

    const std::optional<SettingId> existing_dialect =
        runtime.setting_definitions().find("language.c-family.dialect");
    if (existing_dialect) {
        const SettingRegistry::Definition& definition =
            runtime.setting_definitions().definition(*existing_dialect);
        const SettingScopeMask required_scopes =
            SettingScope::Language | SettingScope::Project | SettingScope::Buffer;
        if (!std::holds_alternative<std::string>(definition.default_value) ||
            (definition.scopes & required_scopes) != required_scopes) {
            throw std::logic_error("C-family dialect setting has an incompatible definition");
        }
    }
    const SettingId dialect =
        existing_dialect
            ? *existing_dialect
            : runtime.setting_definitions().define("language.c-family.dialect", std::string("c++"),
                                                   SettingScope::Language | SettingScope::Project |
                                                       SettingScope::Buffer);
    return {.dialect = dialect,
            .lexer = provider("cind.c-family.lexer", LanguageFacet::Lexing),
            .syntax = provider("cind.c-family.syntax", LanguageFacet::Syntax),
            .indentation = provider("cind.c-family.indentation", LanguageFacet::Indentation),
            .structural_motion =
                provider("cind.c-family.structural-motion", LanguageFacet::StructuralMotion),
            .structural_editing =
                provider("cind.c-family.structural-editing", LanguageFacet::StructuralEditing),
            .highlighting = provider("cind.c-family.highlighting", LanguageFacet::Highlighting)};
}

} // namespace cind
