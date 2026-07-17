#pragma once

#include "cpp_lexer/token.hpp"

#include <cstdint>

namespace cind::ui {

// Semantic style classes for scene runs. Presenters map these to concrete
// colors (SGR sequences, Skia paints, ...); the compositor never deals in
// color values.
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
    StatusBar,
    StatusKey,    // keystroke caption on the status bar
    Message,      // message / prompt line
    Popup,        // candidate and which-key popup rows
    PositionHint, // transient document-position label supplied by input policy
};

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
