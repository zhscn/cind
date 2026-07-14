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

namespace {

// Text range of a node spanning half-open token range [base, base+width).
// Zero-width nodes (MissingToken) collapse to a zero-length range.
TextRange span_range(std::uint32_t base, std::uint32_t width, const std::vector<Token>& toks) {
    if (width == 0) {
        const std::uint32_t offset = base < toks.size()
                                         ? toks[base].range.start.value
                                         : (toks.empty() ? 0 : toks.back().range.end.value);
        return make_range(offset, offset);
    }
    return TextRange{toks[base].range.start, toks[base + width - 1].range.end};
}

} // namespace

SyntaxNodeId SyntaxTree::root() const {
    if (red_.empty() && green_root_) {
        red_.push_back(SyntaxNode{green_root_->kind, 0, green_root_->width, kInvalidNode, {},
                                  green_root_->incomplete, green_root_->reclassified,
                                  green_root_->expected, green_root_.get(), false});
    }
    return 0;
}

void SyntaxTree::expand(SyntaxNodeId id) const {
    SyntaxNode& n = red_[id];
    if (n.expanded) {
        return;
    }
    n.expanded = true;
    const GreenNode* g = n.green;
    std::uint32_t cursor = n.first_token;
    std::vector<SyntaxNodeId> kids;
    kids.reserve(g->children.size());
    for (const GreenChild& gc : g->children) {
        const std::uint32_t cf = cursor + gc.leading;
        kids.push_back(static_cast<SyntaxNodeId>(red_.size()));
        red_.push_back(SyntaxNode{gc.node->kind, cf, cf + gc.node->width, id, {},
                                  gc.node->incomplete, gc.node->reclassified, gc.node->expected,
                                  gc.node.get(), false});
        cursor = cf + gc.node->width;
    }
    red_[id].children = std::move(kids);
}

const SyntaxNode& SyntaxTree::node(SyntaxNodeId id) const {
    expand(id);
    return red_[id];
}

TextRange SyntaxTree::node_range(SyntaxNodeId id) const {
    const SyntaxNode& n = red_[id];
    return span_range(n.first_token, n.end_token - n.first_token, tokens_);
}

SyntaxNodeId SyntaxTree::node_at(TextOffset offset) const {
    SyntaxNodeId current = root();
    while (true) {
        expand(current);
        bool descended = false;
        for (SyntaxNodeId child : red_[current].children) {
            if (red_[child].kind == SyntaxKind::MissingToken) {
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

// Preorder dump straight from the green tree (no red materialization). `base` is
// the node's first token index; child offsets accumulate from relative leadings.
void dump_green(const GreenNode* g, std::uint32_t base, const std::vector<Token>& toks, int depth,
                std::string& out) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    if (g->kind == SyntaxKind::MissingToken) {
        out += "Missing(";
        out += token_kind_name(g->expected);
        out += ")";
    } else {
        out += syntax_kind_name(g->kind);
    }
    const TextRange range = span_range(base, g->width, toks);
    out += " ";
    out += std::to_string(range.start.value);
    out += "..";
    out += std::to_string(range.end.value);
    if (g->incomplete) {
        out += " (incomplete)";
    }
    out += "\n";
    std::uint32_t cursor = base;
    for (const GreenChild& c : g->children) {
        const std::uint32_t cf = cursor + c.leading;
        dump_green(c.node.get(), cf, toks, depth + 1, out);
        cursor = cf + c.node->width;
    }
}

} // namespace

std::string SyntaxTree::dump(std::string_view) const {
    std::string out;
    if (green_root_) {
        dump_green(green_root_.get(), 0, tokens_, 0, out);
    }
    return out;
}

} // namespace cind
