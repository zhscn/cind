#include "syntax/syntax_tree.hpp"

#include <string>

namespace cind {

std::string_view syntax_kind_name(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::TranslationUnit: return "TranslationUnit";
    case SyntaxKind::PreprocessorDirective: return "PreprocessorDirective";
    case SyntaxKind::NamespaceDecl: return "NamespaceDecl";
    case SyntaxKind::NamespaceBody: return "NamespaceBody";
    case SyntaxKind::ClassDecl: return "ClassDecl";
    case SyntaxKind::ClassBody: return "ClassBody";
    case SyntaxKind::AccessSpecifierLabel: return "AccessSpecifierLabel";
    case SyntaxKind::OpaqueDeclaration: return "OpaqueDeclaration";
    case SyntaxKind::FunctionDefinition: return "FunctionDefinition";
    case SyntaxKind::CtorInitializerList: return "CtorInitializerList";
    case SyntaxKind::CtorInitializer: return "CtorInitializer";
    case SyntaxKind::ParenGroup: return "ParenGroup";
    case SyntaxKind::BracketGroup: return "BracketGroup";
    case SyntaxKind::BraceGroup: return "BraceGroup";
    case SyntaxKind::TemplateArgumentList: return "TemplateArgumentList";
    case SyntaxKind::CompoundStatement: return "CompoundStatement";
    case SyntaxKind::IfStatement: return "IfStatement";
    case SyntaxKind::ElseClause: return "ElseClause";
    case SyntaxKind::ForStatement: return "ForStatement";
    case SyntaxKind::WhileStatement: return "WhileStatement";
    case SyntaxKind::DoStatement: return "DoStatement";
    case SyntaxKind::SwitchStatement: return "SwitchStatement";
    case SyntaxKind::CaseSection: return "CaseSection";
    case SyntaxKind::CaseLabel: return "CaseLabel";
    case SyntaxKind::MissingToken: return "MissingToken";
    case SyntaxKind::Error: return "Error";
    }
    return "?";
}

TextRange SyntaxTree::node_range(SyntaxNodeId id) const {
    const SyntaxNode& n = nodes_[id];
    if (n.first_token >= n.end_token) {
        const std::uint32_t offset = n.first_token < tokens_.size()
                                         ? tokens_[n.first_token].range.start.value
                                         : (tokens_.empty() ? 0 : tokens_.back().range.end.value);
        return make_range(offset, offset);
    }
    return TextRange{tokens_[n.first_token].range.start, tokens_[n.end_token - 1].range.end};
}

SyntaxNodeId SyntaxTree::node_at(TextOffset offset) const {
    SyntaxNodeId current = root();
    while (true) {
        bool descended = false;
        for (SyntaxNodeId child : nodes_[current].children) {
            if (nodes_[child].kind == SyntaxKind::MissingToken) {
                continue;
            }
            if (node_range(child).contains(offset)) {
                current = child;
                descended = true;
                break;
            }
        }
        if (!descended) {
            return current;
        }
    }
}

namespace {

void dump_node(const SyntaxTree& tree, SyntaxNodeId id, int depth, std::string& out) {
    const SyntaxNode& n = tree.node(id);
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    if (n.kind == SyntaxKind::MissingToken) {
        out += "Missing(";
        out += token_kind_name(n.expected);
        out += ")";
    } else {
        out += syntax_kind_name(n.kind);
    }
    TextRange range = tree.node_range(id);
    out += " ";
    out += std::to_string(range.start.value);
    out += "..";
    out += std::to_string(range.end.value);
    if (n.incomplete) {
        out += " (incomplete)";
    }
    out += "\n";
    for (SyntaxNodeId child : n.children) {
        dump_node(tree, child, depth + 1, out);
    }
}

} // namespace

std::string SyntaxTree::dump(std::string_view) const {
    std::string out;
    dump_node(*this, root(), 0, out);
    return out;
}

} // namespace cind
