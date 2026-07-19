#pragma once

#include "document/document.hpp"
#include "document/snapshot.hpp"
#include "editor/language.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace cind {

struct Analysis;
class Document;
struct DocumentChange;
struct CppIndentStyle;
struct EnterResult;
struct IndentDecision;
struct TypeCharsResult;

enum class StructuralMotion : std::uint8_t {
    ForwardExpression,
    BackwardExpression,
    UpList,
};

enum class StructuralEdit : std::uint8_t {
    Splice,
    ForwardSlurp,
    ForwardBarf,
    BackwardSlurp,
    BackwardBarf,
};

struct StructuralEditResult {
    TextOffset caret;
    DocumentChange change;
};

// Per-EditSession state for one native language mechanism. The default
// operations reject unsupported use; concrete mechanisms override the
// operations advertised by LanguageMechanism::facets().
class LanguageMechanismSession {
public:
    virtual ~LanguageMechanismSession() = default;

    virtual const Analysis& analysis(const DocumentSnapshot& snapshot);
    virtual void apply(const DocumentChange& change, const DocumentSnapshot& snapshot);
    virtual TypeCharsResult type_chars(Document& document, std::span<const TextOffset> carets,
                                       char character, const CppIndentStyle& style);
    virtual EnterResult newline(Document& document, TextOffset caret, const CppIndentStyle& style);
    virtual IndentDecision indent_line(Document& document, std::uint32_t line,
                                       const CppIndentStyle& style);
    virtual IndentDecision explain_indent(const DocumentSnapshot& snapshot, std::uint32_t line,
                                          const CppIndentStyle& style);
    virtual std::optional<TextOffset> move_structurally(const DocumentSnapshot& snapshot,
                                                        TextOffset from, StructuralMotion motion);
    virtual std::expected<StructuralEditResult, std::string>
    edit_structure(Document& document, TextOffset caret, StructuralEdit edit);
};

// Immutable executable implementation shared by provider declarations. One
// mechanism may implement several facets; an EditSession then shares one
// state object across those provider bindings.
class LanguageMechanism {
public:
    using OpenSession = std::function<std::unique_ptr<LanguageMechanismSession>()>;

    LanguageMechanism(LanguageFacetMask facets, OpenSession open_session);

    bool supports(LanguageFacet facet) const;
    LanguageFacetMask facets() const { return facets_; }
    std::unique_ptr<LanguageMechanismSession> open_session() const;

private:
    LanguageFacetMask facets_ = 0;
    OpenSession open_session_;
};

} // namespace cind
