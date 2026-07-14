#include "syntax/structure.hpp"

#include <algorithm>

namespace cind {

namespace {

bool is_group_kind(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::ParenGroup:
    case SyntaxKind::BracketGroup:
    case SyntaxKind::BraceGroup:
    case SyntaxKind::TemplateArgumentList:
    case SyntaxKind::CompoundStatement:
    case SyntaxKind::ClassBody:
    case SyntaxKind::NamespaceBody: return true;
    default: return false;
    }
}

bool is_open_bracket(TokenKind kind) {
    return kind == TokenKind::LParen || kind == TokenKind::LBracket ||
           kind == TokenKind::LBrace;
}

bool is_close_bracket(TokenKind kind) {
    return kind == TokenKind::RParen || kind == TokenKind::RBracket ||
           kind == TokenKind::RBrace;
}

TokenKind matching_close(TokenKind open) {
    switch (open) {
    case TokenKind::LParen: return TokenKind::RParen;
    case TokenKind::LBracket: return TokenKind::RBracket;
    default: return TokenKind::RBrace;
    }
}

TokenKind matching_open(TokenKind close) {
    switch (close) {
    case TokenKind::RParen: return TokenKind::LParen;
    case TokenKind::RBracket: return TokenKind::LBracket;
    default: return TokenKind::LBrace;
    }
}

bool skippable(const Token& token, bool comments_are_units) {
    if (token.kind == TokenKind::EndOfFile) {
        return false;
    }
    if (comments_are_units &&
        (token.kind == TokenKind::LineComment || token.kind == TokenKind::BlockComment)) {
        return false;
    }
    return is_trivia(token.kind);
}

// The template group whose '<' or final '>'/'>>' is token `index`, if any.
std::optional<TextRange> template_group_at(const SyntaxTree& tree, std::uint32_t index,
                                           bool at_open) {
    const Token& token = tree.tokens()[index];
    for (SyntaxNodeId id = tree.node_at(token.range.start); id != kInvalidNode;
         id = tree.node(id).parent) {
        const SyntaxNode& node = tree.node(id);
        if (node.kind != SyntaxKind::TemplateArgumentList) {
            continue;
        }
        if ((at_open && node.first_token == index) ||
            (!at_open && node.end_token == index + 1)) {
            return tree.node_range(id);
        }
    }
    return std::nullopt;
}

// First token whose range end is beyond `offset`.
std::uint32_t token_at_or_after(const SyntaxTree& tree, TextOffset offset) {
    const auto& tokens = tree.tokens();
    auto it = std::ranges::lower_bound(tokens, offset, std::ranges::less_equal{},
                                       [](const Token& t) { return t.range.end; });
    return static_cast<std::uint32_t>(it - tokens.begin());
}

// One whole unit starting at non-trivia token `i`; nullopt when the token is
// a closer of an enclosing group (motion is blocked).
std::optional<TextRange> unit_at(const SyntaxTree& tree, std::uint32_t i) {
    const auto& tokens = tree.tokens();
    const Token& token = tokens[i];

    if (is_open_bracket(token.kind)) {
        const TokenKind close = matching_close(token.kind);
        int depth = 1;
        for (std::uint32_t j = i + 1; j < tokens.size(); ++j) {
            if (tokens[j].kind == token.kind) {
                ++depth;
            } else if (tokens[j].kind == close && --depth == 0) {
                return TextRange{token.range.start, tokens[j].range.end};
            }
        }
        // Unterminated (mid-typing): the unit soft-extends to the end.
        return TextRange{token.range.start, tokens.back().range.end};
    }
    if (is_close_bracket(token.kind)) {
        return std::nullopt;
    }
    if (token.kind == TokenKind::Less) {
        if (auto group = template_group_at(tree, i, true)) {
            return group;
        }
    }
    if (token.kind == TokenKind::Greater || token.kind == TokenKind::GreaterGreater) {
        if (template_group_at(tree, i, false)) {
            return std::nullopt; // closes an enclosing template group
        }
    }
    return token.range;
}

} // namespace

std::optional<TextRange> sexp_forward(const SyntaxTree& tree, TextOffset from) {
    const auto& tokens = tree.tokens();
    for (std::uint32_t i = token_at_or_after(tree, from); i < tokens.size(); ++i) {
        const Token& token = tokens[i];
        if (token.kind == TokenKind::EndOfFile) {
            return std::nullopt;
        }
        if (skippable(token, false) || token.kind == TokenKind::LineComment ||
            token.kind == TokenKind::BlockComment) {
            continue;
        }
        if (token.range.start < from) {
            // Mid-atom: move to the end of the current atom.
            return TextRange{from, token.range.end};
        }
        return unit_at(tree, i);
    }
    return std::nullopt;
}

std::optional<TextRange> sexp_backward(const SyntaxTree& tree, TextOffset from) {
    const auto& tokens = tree.tokens();
    // First token wholly past `from`; everything before it is a candidate.
    auto it = std::ranges::upper_bound(tokens, from, {},
                                       [](const Token& t) { return t.range.end; });
    if (it != tokens.end() && it->range.start < from && !is_trivia(it->kind) &&
        it->kind != TokenKind::EndOfFile) {
        // Mid-atom: move to the start of the current atom.
        return TextRange{it->range.start, from};
    }
    while (it != tokens.begin()) {
        --it;
        const Token& token = *it;
        if (is_trivia(token.kind) || token.kind == TokenKind::EndOfFile) {
            continue;
        }
        const auto i = static_cast<std::uint32_t>(it - tokens.begin());
        if (is_close_bracket(token.kind)) {
            const TokenKind open = matching_open(token.kind);
            int depth = 1;
            for (std::uint32_t j = i; j-- > 0;) {
                if (tokens[j].kind == token.kind) {
                    ++depth;
                } else if (tokens[j].kind == open && --depth == 0) {
                    return TextRange{tokens[j].range.start, token.range.end};
                }
            }
            return TextRange{tokens.front().range.start, token.range.end};
        }
        if (is_open_bracket(token.kind)) {
            return std::nullopt; // blocked by the enclosing opener
        }
        if (token.kind == TokenKind::Greater || token.kind == TokenKind::GreaterGreater) {
            if (auto group = template_group_at(tree, i, false)) {
                return group;
            }
        }
        if (token.kind == TokenKind::Less && template_group_at(tree, i, true)) {
            return std::nullopt;
        }
        return token.range;
    }
    return std::nullopt;
}

std::optional<TextRange> enclosing_list(const SyntaxTree& tree, TextOffset offset) {
    for (SyntaxNodeId id = tree.node_at(offset); id != kInvalidNode;
         id = tree.node(id).parent) {
        if (!is_group_kind(tree.node(id).kind)) {
            continue;
        }
        TextRange range = tree.node_range(id);
        // Only a group we are strictly inside of (not sitting on its edge).
        if (range.start < offset && offset < range.end) {
            return range;
        }
    }
    return std::nullopt;
}

std::optional<TextRange> expand_selection(const SyntaxTree& tree, TextRange range) {
    const auto& tokens = tree.tokens();
    if (range.empty()) {
        // Seed: the non-trivia token at the position.
        const std::uint32_t i = token_at_or_after(tree, range.start);
        if (i < tokens.size() && !is_trivia(tokens[i].kind) &&
            tokens[i].kind != TokenKind::EndOfFile &&
            tokens[i].range.start <= range.start) {
            return tokens[i].range;
        }
    }
    auto strictly_contains = [&](TextRange outer) {
        return outer.start <= range.start && range.end <= outer.end &&
               outer.length() > range.length();
    };
    for (SyntaxNodeId id = tree.node_at(range.start); id != kInvalidNode;
         id = tree.node(id).parent) {
        const SyntaxNode& node = tree.node(id);
        if (is_group_kind(node.kind) && node.end_token - node.first_token >= 2) {
            // Interior first (content between the delimiters), then the
            // whole group.
            TextRange inner{tokens[node.first_token].range.end,
                            tokens[node.end_token - 1].range.start};
            if (strictly_contains(inner)) {
                return inner;
            }
        }
        TextRange full = tree.node_range(id);
        if (strictly_contains(full)) {
            return full;
        }
    }
    return std::nullopt;
}

TextRange soft_kill_end(const SyntaxTree& tree, const Text& text, TextOffset from) {
    const std::uint32_t line = text.position(from).line;
    const TextOffset content_end = text.line_content_end(line);
    if (from >= content_end) {
        // At (or past) the line's content: kill through the newline.
        return TextRange{from, text.line_range(line).end};
    }

    const auto& tokens = tree.tokens();
    TextOffset end = from;
    for (std::uint32_t i = token_at_or_after(tree, from); i < tokens.size(); ++i) {
        const Token& token = tokens[i];
        if (token.kind == TokenKind::EndOfFile || token.range.start >= content_end) {
            break;
        }
        if (token.range.start < from) {
            end = std::max(end, token.range.end); // mid-atom: finish the atom
            continue;
        }
        if (skippable(token, true)) {
            continue;
        }
        std::optional<TextRange> unit = unit_at(tree, i);
        if (!unit) {
            break; // the enclosing closer: never cross it
        }
        end = std::max(end, unit->end);
        // Skip everything the unit swallowed.
        while (i + 1 < tokens.size() && tokens[i + 1].range.start < unit->end) {
            ++i;
        }
    }
    return TextRange{from, end};
}

} // namespace cind
