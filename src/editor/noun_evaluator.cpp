#include "editor/noun_evaluator.hpp"

#include "document/document.hpp"
#include "syntax/structure.hpp"
#include "syntax/syntax_tree.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cind {

namespace {

enum class CharacterClass : std::uint8_t {
    Whitespace,
    Word,
    Symbol,
};

CharacterClass character_class(unsigned char byte, bool symbol_words) {
    if (std::isspace(byte) != 0) {
        return CharacterClass::Whitespace;
    }
    if (std::isalnum(byte) != 0 || byte == '_' || byte >= 0x80 ||
        (symbol_words && (byte == '-' || byte == '.'))) {
        return CharacterClass::Word;
    }
    return CharacterClass::Symbol;
}

SelectionRange selection_range(TextRange range, SelectionGranularity granularity) {
    return {.anchor = range.start, .head = range.end, .granularity = granularity};
}

std::optional<SyntaxKind> syntax_kind(std::string_view name) {
    if (name == "translation-unit")
        return SyntaxKind::TranslationUnit;
    if (name == "preprocessor-directive")
        return SyntaxKind::PreprocessorDirective;
    if (name == "namespace-declaration")
        return SyntaxKind::NamespaceDecl;
    if (name == "namespace-body")
        return SyntaxKind::NamespaceBody;
    if (name == "class-declaration")
        return SyntaxKind::ClassDecl;
    if (name == "class-body")
        return SyntaxKind::ClassBody;
    if (name == "function-definition")
        return SyntaxKind::FunctionDefinition;
    if (name == "parenthesized")
        return SyntaxKind::ParenGroup;
    if (name == "bracketed")
        return SyntaxKind::BracketGroup;
    if (name == "braced")
        return SyntaxKind::BraceGroup;
    if (name == "template-arguments")
        return SyntaxKind::TemplateArgumentList;
    if (name == "compound-statement")
        return SyntaxKind::CompoundStatement;
    if (name == "if-statement")
        return SyntaxKind::IfStatement;
    if (name == "for-statement")
        return SyntaxKind::ForStatement;
    if (name == "while-statement")
        return SyntaxKind::WhileStatement;
    if (name == "do-statement")
        return SyntaxKind::DoStatement;
    if (name == "switch-statement")
        return SyntaxKind::SwitchStatement;
    return std::nullopt;
}

std::optional<ThingMatch> cst_match(std::string_view name, const DocumentSnapshot& snapshot,
                                    const SyntaxTree& tree, TextOffset position) {
    if (name == "string-literal") {
        for (const Token& token : tree.tokens()) {
            if (token.range.start > position || position > token.range.end) {
                continue;
            }
            if (token.kind != TokenKind::StringLiteral &&
                token.kind != TokenKind::RawStringLiteral &&
                token.kind != TokenKind::CharacterLiteral) {
                continue;
            }
            const std::string literal = snapshot.substring(token.range);
            const std::size_t first = literal.find_first_of("\"'");
            const std::size_t last = literal.find_last_of("\"'");
            TextRange inner = token.range;
            if (first != std::string::npos && last != std::string::npos && first < last) {
                inner.start.value += static_cast<std::uint32_t>(first + 1);
                inner.end = TextOffset{token.range.start.value + static_cast<std::uint32_t>(last)};
            }
            return ThingMatch{.inner = selection_range(inner, SelectionGranularity::Character),
                              .bounds = selection_range(token.range, SelectionGranularity::Node)};
        }
        return std::nullopt;
    }
    const std::optional<SyntaxKind> expected = syntax_kind(name);
    if (!expected) {
        return std::nullopt;
    }
    for (SyntaxNodeId node = tree.node_at(position); node != kInvalidNode;
         node = tree.node(node).parent) {
        if (tree.node(node).kind == *expected) {
            const TextRange range = tree.node_range(node);
            const SelectionRange selected = selection_range(range, SelectionGranularity::Node);
            return ThingMatch{.inner = selected, .bounds = selected};
        }
    }
    return std::nullopt;
}

std::optional<ThingMatch> structural_pair_match(std::string_view open, std::string_view close,
                                                const DocumentSnapshot& snapshot,
                                                const SyntaxTree& tree, TextOffset position) {
    for (SyntaxNodeId node = tree.node_at(position); node != kInvalidNode;
         node = tree.node(node).parent) {
        const SyntaxNode& candidate = tree.node(node);
        if (candidate.end_token <= candidate.first_token + 1) {
            continue;
        }
        const Token& first = tree.tokens()[candidate.first_token];
        const Token& last = tree.tokens()[candidate.end_token - 1];
        if (snapshot.substring(first.range) != open || snapshot.substring(last.range) != close) {
            continue;
        }
        const TextRange bounds{first.range.start, last.range.end};
        const TextRange inner{first.range.end, last.range.start};
        return ThingMatch{.inner = selection_range(inner, SelectionGranularity::Character),
                          .bounds = selection_range(bounds, SelectionGranularity::Node)};
    }
    return std::nullopt;
}

std::optional<ThingMatch> textual_pair_match(std::string_view open, std::string_view close,
                                             const DocumentSnapshot& snapshot,
                                             TextOffset position) {
    const std::string text = snapshot.content().to_string();
    const std::size_t at = std::min<std::size_t>(position.value, text.size());
    std::size_t candidate = text.rfind(open, at);
    while (candidate != std::string::npos) {
        std::size_t cursor = candidate + open.size();
        std::size_t depth = 1;
        while (cursor <= text.size()) {
            const std::size_t next_open = text.find(open, cursor);
            const std::size_t next_close = text.find(close, cursor);
            if (next_close == std::string::npos) {
                break;
            }
            if (next_open != std::string::npos && next_open < next_close) {
                ++depth;
                cursor = next_open + open.size();
                continue;
            }
            --depth;
            const std::size_t end = next_close + close.size();
            if (depth == 0) {
                if (candidate <= at && at <= end) {
                    const TextRange bounds = make_range(static_cast<std::uint32_t>(candidate),
                                                        static_cast<std::uint32_t>(end));
                    const TextRange inner =
                        make_range(static_cast<std::uint32_t>(candidate + open.size()),
                                   static_cast<std::uint32_t>(next_close));
                    return ThingMatch{
                        .inner = selection_range(inner, SelectionGranularity::Character),
                        .bounds = selection_range(bounds, SelectionGranularity::Node)};
                }
                break;
            }
            cursor = end;
        }
        if (candidate == 0) {
            break;
        }
        candidate = text.rfind(open, candidate - 1);
    }
    return std::nullopt;
}

std::optional<ThingMatch> character_match(std::string_view name, const DocumentSnapshot& snapshot,
                                          TextOffset position) {
    const std::string text = snapshot.content().to_string();
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t at = std::min<std::size_t>(position.value, text.size() - 1);
    const bool symbol_words = name == "symbol";
    CharacterClass target = character_class(static_cast<unsigned char>(text[at]), symbol_words);
    if (target == CharacterClass::Whitespace && position.value > 0) {
        const CharacterClass previous =
            character_class(static_cast<unsigned char>(text[position.value - 1]), symbol_words);
        if (previous != CharacterClass::Whitespace) {
            --at;
            target = previous;
        }
    }
    if (target == CharacterClass::Whitespace) {
        return std::nullopt;
    }
    std::size_t start = at;
    while (start > 0 &&
           character_class(static_cast<unsigned char>(text[start - 1]), symbol_words) == target) {
        --start;
    }
    std::size_t end = at + 1;
    while (end < text.size() &&
           character_class(static_cast<unsigned char>(text[end]), symbol_words) == target) {
        ++end;
    }
    const SelectionRange selected = selection_range(
        make_range(static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(end)),
        SelectionGranularity::Character);
    return ThingMatch{.inner = selected, .bounds = selected};
}

std::optional<ThingMatch> evaluate_pattern(const ThingPattern& pattern,
                                           const DocumentSnapshot& snapshot, const SyntaxTree& tree,
                                           TextOffset position) {
    switch (pattern.kind) {
    case ThingPatternKind::Pair:
        if (const std::optional<ThingMatch> structural = structural_pair_match(
                pattern.arguments[0], pattern.arguments[1], snapshot, tree, position)) {
            return structural;
        }
        return textual_pair_match(pattern.arguments[0], pattern.arguments[1], snapshot, position);
    case ThingPatternKind::CstNode:
        return cst_match(pattern.arguments[0], snapshot, tree, position);
    case ThingPatternKind::CharacterClass:
        return character_match(pattern.arguments[0], snapshot, position);
    case ThingPatternKind::Multi:
        for (const ThingPattern& alternative : pattern.alternatives) {
            if (std::optional<ThingMatch> match =
                    evaluate_pattern(alternative, snapshot, tree, position)) {
                return match;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

MotionMechanism reversed(MotionMechanism mechanism) {
    switch (mechanism) {
    case MotionMechanism::ForwardCharacter:
        return MotionMechanism::BackwardCharacter;
    case MotionMechanism::BackwardCharacter:
        return MotionMechanism::ForwardCharacter;
    case MotionMechanism::ForwardWord:
        return MotionMechanism::BackwardWord;
    case MotionMechanism::BackwardWord:
        return MotionMechanism::ForwardWord;
    case MotionMechanism::ForwardExpression:
        return MotionMechanism::BackwardExpression;
    case MotionMechanism::BackwardExpression:
        return MotionMechanism::ForwardExpression;
    case MotionMechanism::UpList:
        return MotionMechanism::UpList;
    }
    return mechanism;
}

TextOffset forward_word(const Text& text, TextOffset from) {
    const std::string content = text.to_string();
    std::size_t at = std::min<std::size_t>(from.value, content.size());
    if (at == content.size()) {
        return from;
    }
    const CharacterClass initial = character_class(static_cast<unsigned char>(content[at]), false);
    if (initial != CharacterClass::Whitespace) {
        while (at < content.size() &&
               character_class(static_cast<unsigned char>(content[at]), false) == initial) {
            ++at;
        }
    }
    while (at < content.size() && character_class(static_cast<unsigned char>(content[at]), false) ==
                                      CharacterClass::Whitespace) {
        ++at;
    }
    return TextOffset{static_cast<std::uint32_t>(at)};
}

TextOffset backward_word(const Text& text, TextOffset from) {
    const std::string content = text.to_string();
    std::size_t at = std::min<std::size_t>(from.value, content.size());
    while (at > 0 && character_class(static_cast<unsigned char>(content[at - 1]), false) ==
                         CharacterClass::Whitespace) {
        --at;
    }
    if (at == 0) {
        return TextOffset{0};
    }
    const CharacterClass target =
        character_class(static_cast<unsigned char>(content[at - 1]), false);
    while (at > 0 &&
           character_class(static_cast<unsigned char>(content[at - 1]), false) == target) {
        --at;
    }
    return TextOffset{static_cast<std::uint32_t>(at)};
}

TextOffset move_once(MotionMechanism mechanism, const DocumentSnapshot& snapshot,
                     const SyntaxTree& tree, TextOffset from) {
    switch (mechanism) {
    case MotionMechanism::ForwardCharacter:
        return ui::next_grapheme(snapshot.content(), from);
    case MotionMechanism::BackwardCharacter:
        return ui::previous_grapheme(snapshot.content(), from);
    case MotionMechanism::ForwardWord:
        return forward_word(snapshot.content(), from);
    case MotionMechanism::BackwardWord:
        return backward_word(snapshot.content(), from);
    case MotionMechanism::ForwardExpression:
        if (const std::optional<TextRange> range = sexp_forward(tree, from))
            return range->end;
        return from;
    case MotionMechanism::BackwardExpression:
        if (const std::optional<TextRange> range = sexp_backward(tree, from))
            return range->start;
        return from;
    case MotionMechanism::UpList:
        if (const std::optional<TextRange> range = enclosing_list(tree, from))
            return range->start;
        return from;
    }
    return from;
}

} // namespace

std::optional<ThingMatch> evaluate_thing(const ThingRegistry& registry, ThingId thing,
                                         const DocumentSnapshot& snapshot, const SyntaxTree& tree,
                                         TextOffset position) {
    return evaluate_pattern(registry.definition(thing).pattern, snapshot, tree, position);
}

ViewSelection evaluate_motion(const MotionRegistry& registry, MotionId motion,
                              const DocumentSnapshot& snapshot, const SyntaxTree& tree,
                              const ViewSelection& selection, std::int64_t count, bool extend) {
    MotionMechanism mechanism = registry.definition(motion).mechanism;
    if (count < 0) {
        mechanism = reversed(mechanism);
    }
    const std::uint64_t magnitude = count == std::numeric_limits<std::int64_t>::min()
                                        ? std::uint64_t{1} << 63U
                                        : static_cast<std::uint64_t>(std::abs(count));
    ViewSelection result = selection;
    for (SelectionRange& range : result.ranges) {
        TextOffset target = range.head;
        for (std::uint64_t step = 0; step < magnitude; ++step) {
            const TextOffset next = move_once(mechanism, snapshot, tree, target);
            if (next == target) {
                break;
            }
            target = next;
        }
        if (!extend) {
            range.anchor = target;
        }
        range.head = target;
        range.granularity = SelectionGranularity::Character;
    }
    return result;
}

std::optional<ViewSelection> evaluate_node_expansion(const SyntaxTree& tree,
                                                     const ViewSelection& selection) {
    ViewSelection result = selection;
    for (SelectionRange& range : result.ranges) {
        const std::optional<TextRange> expanded = expand_selection(tree, range.ordered());
        if (!expanded) {
            return std::nullopt;
        }
        const bool reversed = range.head < range.anchor;
        range.anchor = reversed ? expanded->end : expanded->start;
        range.head = reversed ? expanded->start : expanded->end;
        range.granularity = SelectionGranularity::Node;
    }
    return result;
}

} // namespace cind
