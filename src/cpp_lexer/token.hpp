#pragma once

#include "document/text_types.hpp"

#include <cstdint>
#include <string_view>

namespace cind {

enum class TokenKind : std::uint8_t {
    Whitespace,
    Newline,
    LineComment,
    BlockComment,

    Identifier,
    Number,
    StringLiteral,
    RawStringLiteral,
    CharacterLiteral,

    // Indentation-relevant keywords only; every other keyword stays
    // Identifier and is classified by the parser on demand.
    NamespaceKw,
    ClassKw,
    StructKw,
    EnumKw,
    UnionKw,
    SwitchKw,
    CaseKw,
    DefaultKw,
    PublicKw,
    ProtectedKw,
    PrivateKw,
    IfKw,
    ElseKw,
    ForKw,
    WhileKw,
    DoKw,
    ReturnKw,
    TemplateKw,
    OperatorKw,

    LBrace,
    RBrace,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Less,
    Greater,
    Colon,
    ColonColon,
    Comma,
    Semicolon,
    Arrow,
    Equals,

    // Operator punctuators, one kind per spelling (maximal munch). The
    // block from Plus to GreaterGreaterEqual is contiguous — see
    // is_operator_spelling().
    Plus,                // +
    Minus,               // -
    Star,                // *
    Slash,               // /
    Percent,             // %
    Exclaim,             // !
    Amp,                 // &
    Pipe,                // |
    Caret,               // ^
    Tilde,               // ~
    Question,            // ?
    Period,              // .
    Ellipsis,            // ...
    PlusPlus,            // ++
    MinusMinus,          // --
    AmpAmp,              // &&
    PipePipe,            // ||
    EqualEqual,          // ==
    ExclaimEqual,        // !=
    LessEqual,           // <=
    GreaterEqual,        // >=
    Spaceship,           // <=>
    LessLess,            // <<
    GreaterGreater,      // >>
    PeriodStar,          // .*
    ArrowStar,           // ->*
    PlusEqual,           // +=
    MinusEqual,          // -=
    StarEqual,           // *=
    SlashEqual,          // /=
    PercentEqual,        // %=
    AmpEqual,            // &=
    PipeEqual,           // |=
    CaretEqual,          // ^=
    LessLessEqual,       // <<=
    GreaterGreaterEqual, // >>=

    // Leftover operator spellings ('##', stringizing '#'); opaque.
    Punctuator,

    PreprocessorHash,
    Invalid,
    EndOfFile,
};

enum class LexicalFlags : std::uint8_t {
    None = 0,
    // Token belongs to a preprocessor directive line.
    PreprocessorLine = 1 << 0,
    // Literal or block comment missing its terminator.
    Unterminated = 1 << 1,
    // Whitespace token that is a backslash-newline splice.
    EscapedNewline = 1 << 2,
};

constexpr LexicalFlags operator|(LexicalFlags a, LexicalFlags b) {
    return static_cast<LexicalFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr LexicalFlags& operator|=(LexicalFlags& a, LexicalFlags b) { return a = a | b; }
constexpr bool has_flag(LexicalFlags flags, LexicalFlags flag) {
    return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(flag)) != 0;
}

struct Token {
    TokenKind kind;
    TextRange range;
    LexicalFlags flags = LexicalFlags::None;
};

// True for the operator-punctuator block above (excludes structural tokens
// like Less/Greater/Equals/Arrow that predate it).
constexpr bool is_operator_spelling(TokenKind kind) {
    return kind >= TokenKind::Plus && kind <= TokenKind::GreaterGreaterEqual;
}

constexpr bool is_trivia(TokenKind kind) {
    return kind == TokenKind::Whitespace || kind == TokenKind::Newline ||
           kind == TokenKind::LineComment || kind == TokenKind::BlockComment;
}

std::string_view token_kind_name(TokenKind kind);

} // namespace cind
