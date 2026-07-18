#include "editor/language_mechanism.hpp"

#include "commands/editor_commands.hpp"

#include <stdexcept>
#include <utility>

namespace cind {

namespace {

[[noreturn]] void unavailable(std::string_view operation) {
    throw std::logic_error(std::string(operation) + " is unavailable in this language mechanism");
}

} // namespace

const Analysis& LanguageMechanismSession::analysis(const DocumentSnapshot&) {
    unavailable("syntax analysis");
}

void LanguageMechanismSession::apply(const DocumentChange&, const DocumentSnapshot&) {}

TypeCharsResult LanguageMechanismSession::type_chars(Document&, std::span<const TextOffset>, char,
                                                     const CppIndentStyle&) {
    unavailable("structural text input");
}

EnterResult LanguageMechanismSession::newline(Document&, TextOffset, const CppIndentStyle&) {
    unavailable("language newline");
}

IndentDecision LanguageMechanismSession::indent_line(Document&, std::uint32_t,
                                                     const CppIndentStyle&) {
    unavailable("language indentation");
}

IndentDecision LanguageMechanismSession::explain_indent(const DocumentSnapshot&, std::uint32_t,
                                                        const CppIndentStyle&) {
    unavailable("indentation explanation");
}

std::optional<TextOffset>
LanguageMechanismSession::move_structurally(const DocumentSnapshot&, TextOffset, StructuralMotion) {
    unavailable("structural motion");
}

LanguageMechanism::LanguageMechanism(LanguageFacetMask facets, OpenSession open_session)
    : facets_(facets), open_session_(std::move(open_session)) {
    if (facets_ == 0 || (facets_ & ~kAllLanguageFacets) != 0) {
        throw std::invalid_argument("language mechanism has an invalid facet mask");
    }
    if (!open_session_) {
        throw std::invalid_argument("language mechanism requires a session factory");
    }
}

bool LanguageMechanism::supports(LanguageFacet facet) const {
    return facet != LanguageFacet::Count && (facets_ & language_facet_bit(facet)) != 0;
}

std::unique_ptr<LanguageMechanismSession> LanguageMechanism::open_session() const {
    std::unique_ptr<LanguageMechanismSession> session = open_session_();
    if (!session) {
        throw std::logic_error("language mechanism returned an empty session");
    }
    return session;
}

} // namespace cind
