#include "cli/session.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace cind {

EditSession::EditSession(std::string initial_text, CppIndentStyle style)
    : document_(std::move(initial_text)), style_(style) {}

void EditSession::set_caret(TextOffset caret) {
    if (caret.value > snapshot().size_bytes()) {
        throw std::out_of_range("EditSession: caret out of range");
    }
    caret_ = caret;
}

void EditSession::type_text(std::string_view text) {
    // Character by character through the typed-char pipeline, exactly like
    // an editor delivering keystrokes; each character is one undo unit.
    for (char ch : text) {
        const TextOffset before = caret_;
        TypeCharResult result = type_char(document_, caret_, ch, style_);
        caret_ = result.caret;
        undo_carets_.push_back(before);
        redo_carets_.clear();
    }
}

EnterResult EditSession::enter() {
    const TextOffset before = caret_;
    EnterResult result = press_enter(document_, caret_, style_);
    caret_ = result.caret;
    undo_carets_.push_back(before);
    redo_carets_.clear();
    return result;
}

IndentDecision EditSession::indent() {
    DocumentSnapshot snap = snapshot();
    const std::uint32_t line = snap.content().position(caret_).line;
    const TextOffset line_start = snap.content().line_start(line);
    const std::string content = snap.substring(snap.content().line_content_range(line));
    std::uint32_t old_len = 0;
    while (old_len < content.size() && (content[old_len] == ' ' || content[old_len] == '\t')) {
        ++old_len;
    }

    const TextOffset before = caret_;
    const RevisionId revision_before = document_.revision();
    IndentDecision decision = indent_line(document_, line, style_);
    if (document_.revision() != revision_before) {
        const auto new_len = static_cast<std::uint32_t>(decision.indentation_text.size());
        if (caret_.value >= line_start.value + old_len) {
            caret_.value = caret_.value - old_len + new_len;
        } else {
            caret_.value = line_start.value + new_len;
        }
        undo_carets_.push_back(before);
        redo_carets_.clear();
    }
    return decision;
}

bool EditSession::undo() {
    if (!document_.can_undo()) {
        return false;
    }
    document_.undo();
    redo_carets_.push_back(caret_);
    if (!undo_carets_.empty()) {
        caret_ = undo_carets_.back();
        undo_carets_.pop_back();
    }
    caret_.value = std::min(caret_.value, snapshot().size_bytes());
    return true;
}

bool EditSession::redo() {
    if (!document_.can_redo()) {
        return false;
    }
    document_.redo();
    undo_carets_.push_back(caret_);
    if (!redo_carets_.empty()) {
        caret_ = redo_carets_.back();
        redo_carets_.pop_back();
    }
    caret_.value = std::min(caret_.value, snapshot().size_bytes());
    return true;
}

IndentDecision EditSession::explain() const {
    DocumentSnapshot snap = snapshot();
    SyntaxTree tree = parse(snap.content());
    return compute_line_indent(snap, tree, snap.content().position(caret_).line, style_);
}

std::string EditSession::render_with_caret() const {
    std::string out = snapshot().content().to_string();
    out.insert(caret_.value, "^");
    return out;
}

bool set_style_field(CppIndentStyle& style, std::string_view key, std::string_view value) {
    auto parse_int = [&](int& out) {
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
        return ec == std::errc() && ptr == value.data() + value.size();
    };
    auto parse_bool = [&](bool& out) {
        if (value == "true") {
            out = true;
            return true;
        }
        if (value == "false") {
            out = false;
            return true;
        }
        return false;
    };

    if (key == "indent_width") {
        return parse_int(style.indent_width);
    }
    if (key == "continuation_indent") {
        return parse_int(style.continuation_indent);
    }
    if (key == "tab_width") {
        return parse_int(style.tab_width);
    }
    if (key == "access_specifier_offset") {
        return parse_int(style.access_specifier_offset);
    }
    if (key == "use_tabs") {
        return parse_bool(style.use_tabs);
    }
    if (key == "indent_namespace_body" || key == "namespace_indentation") {
        using NI = CppIndentStyle::NamespaceIndentation;
        if (value == "true" || value == "All") {
            style.namespace_indentation = NI::All;
        } else if (value == "false" || value == "None") {
            style.namespace_indentation = NI::None;
        } else if (value == "Inner") {
            style.namespace_indentation = NI::Inner;
        } else {
            return false;
        }
        return true;
    }
    if (key == "indent_type_body") {
        return parse_bool(style.indent_type_body);
    }
    if (key == "indent_case_label") {
        return parse_bool(style.indent_case_label);
    }
    if (key == "indent_case_body") {
        return parse_bool(style.indent_case_body);
    }
    if (key == "constructor_initializers") {
        using Style = CppIndentStyle::ConstructorInitializerStyle;
        if (value == "NormalIndent") {
            style.constructor_initializers = Style::NormalIndent;
        } else if (value == "ContinuationIndent") {
            style.constructor_initializers = Style::ContinuationIndent;
        } else if (value == "AlignFirstInitializer") {
            style.constructor_initializers = Style::AlignFirstInitializer;
        } else if (value == "AlignAfterColon") {
            style.constructor_initializers = Style::AlignAfterColon;
        } else {
            return false;
        }
        return true;
    }
    return false;
}

CaretText split_caret_marker(std::string_view marked) {
    CaretText out;
    std::size_t pos = marked.find('^');
    out.had_marker = pos != std::string_view::npos;
    out.text = std::string(marked);
    if (out.had_marker) {
        out.text.erase(pos, 1);
        out.caret = TextOffset{static_cast<std::uint32_t>(pos)};
    } else {
        out.caret = TextOffset{static_cast<std::uint32_t>(out.text.size())};
    }
    return out;
}

} // namespace cind
