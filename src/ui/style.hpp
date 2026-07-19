#pragma once

#include "cpp_lexer/token.hpp"
#include "presentation/style.hpp"

#include <cstdint>

namespace cind::ui {

// Semantic style classes for scene runs. The scripted presentation policy
// resolves them to concrete attributes; presenters only encode those
// attributes for their backend.
enum class StyleClass : std::uint8_t {
    Text,
    Keyword,
    String,
    Number,
    Comment,
    Preprocessor,
    Gutter,    // line numbers and the "~" past-EOF marker
    SignAdded, // unsaved-change sign column
    SignModified,
    SignDeleted,
    DiagnosticError,
    DiagnosticWarning,
    DiagnosticInformation,
    DiagnosticHint,
    StatusBar,
    StatusKey,    // keystroke caption on the status bar
    Message,      // message / prompt line
    Popup,        // candidate and which-key popup rows
    PositionHint, // transient document-position label supplied by input policy
};

constexpr PresentationTextRole presentation_role(StyleClass style) {
    switch (style) {
    case StyleClass::Text:
        return PresentationTextRole::Text;
    case StyleClass::Keyword:
        return PresentationTextRole::Keyword;
    case StyleClass::String:
        return PresentationTextRole::String;
    case StyleClass::Number:
        return PresentationTextRole::Number;
    case StyleClass::Comment:
        return PresentationTextRole::Comment;
    case StyleClass::Preprocessor:
        return PresentationTextRole::Preprocessor;
    case StyleClass::Gutter:
        return PresentationTextRole::Gutter;
    case StyleClass::SignAdded:
        return PresentationTextRole::SignAdded;
    case StyleClass::SignModified:
        return PresentationTextRole::SignModified;
    case StyleClass::SignDeleted:
        return PresentationTextRole::SignDeleted;
    case StyleClass::DiagnosticError:
        return PresentationTextRole::DiagnosticError;
    case StyleClass::DiagnosticWarning:
        return PresentationTextRole::DiagnosticWarning;
    case StyleClass::DiagnosticInformation:
        return PresentationTextRole::DiagnosticInformation;
    case StyleClass::DiagnosticHint:
        return PresentationTextRole::DiagnosticHint;
    case StyleClass::StatusBar:
        return PresentationTextRole::StatusBar;
    case StyleClass::StatusKey:
        return PresentationTextRole::StatusKey;
    case StyleClass::Message:
        return PresentationTextRole::Message;
    case StyleClass::Popup:
        return PresentationTextRole::Popup;
    case StyleClass::PositionHint:
        return PresentationTextRole::PositionHint;
    }
    return PresentationTextRole::Text;
}

// Layer-1 highlighting straight off the lexer: token kind -> style class.
constexpr StyleClass style_of(const Token& token) {
    if (has_flag(token.flags, LexicalFlags::PreprocessorLine)) {
        return StyleClass::Preprocessor;
    }
    switch (token.kind) {
    case TokenKind::LineComment:
    case TokenKind::BlockComment:
        return StyleClass::Comment;
    case TokenKind::StringLiteral:
    case TokenKind::RawStringLiteral:
    case TokenKind::CharacterLiteral:
        return StyleClass::String;
    case TokenKind::Number:
        return StyleClass::Number;
    case TokenKind::NamespaceKw:
    case TokenKind::ClassKw:
    case TokenKind::StructKw:
    case TokenKind::EnumKw:
    case TokenKind::UnionKw:
    case TokenKind::SwitchKw:
    case TokenKind::CaseKw:
    case TokenKind::DefaultKw:
    case TokenKind::PublicKw:
    case TokenKind::ProtectedKw:
    case TokenKind::PrivateKw:
    case TokenKind::IfKw:
    case TokenKind::ElseKw:
    case TokenKind::ForKw:
    case TokenKind::WhileKw:
    case TokenKind::DoKw:
    case TokenKind::ReturnKw:
    case TokenKind::TemplateKw:
    case TokenKind::OperatorKw:
        return StyleClass::Keyword;
    default:
        return StyleClass::Text;
    }
}

} // namespace cind::ui
