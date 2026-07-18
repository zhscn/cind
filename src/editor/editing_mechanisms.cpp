#include "editor/editing_mechanisms.hpp"

#include "syntax/structure.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cind {

EditingMechanisms::EditingMechanisms(EditSessionResolver session, EditingMechanismHooks hooks)
    : session_(std::move(session)), hooks_(std::move(hooks)) {}

bool EditingMechanisms::undo(ViewId view) {
    const bool changed = session_(view).undo();
    if (changed) {
        notify_edited();
    }
    return changed;
}

bool EditingMechanisms::redo(ViewId view) {
    const bool changed = session_(view).redo();
    if (changed) {
        notify_edited();
    }
    return changed;
}

void EditingMechanisms::reset_preferred_column(ViewId view) {
    session_(view).view().viewport().preferred_column.reset();
}

void EditingMechanisms::move_lines(ViewId view, std::int64_t delta) {
    EditSession& active = session_(view);
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    ViewportState& viewport = active.view().viewport();
    if (!viewport.preferred_column) {
        viewport.preferred_column =
            ui::display_column(text, active.caret(), active.style().tab_width);
    }
    const std::int64_t current_line = text.position(active.caret()).line;
    const std::int64_t last_line = static_cast<std::int64_t>(text.line_count()) - 1;
    const std::int64_t target =
        delta > 0 && current_line > std::numeric_limits<std::int64_t>::max() - delta
            ? last_line
            : std::clamp(current_line + delta, std::int64_t{0}, last_line);
    active.set_caret(ui::offset_at_display_column(
        text, {.line = static_cast<std::uint32_t>(target), .column = *viewport.preferred_column},
        active.style().tab_width));
    notify_caret_moved();
}

void EditingMechanisms::move_line_boundary(ViewId view, bool end) {
    EditSession& active = session_(view);
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    const std::uint32_t line = text.position(active.caret()).line;
    active.set_caret(end ? text.line_content_end(line) : text.line_start(line));
    notify_caret_moved();
}

DeleteGraphemeOutcome EditingMechanisms::delete_grapheme(ViewId view, bool forward,
                                                         DeleteGraphemeMode mode) {
    const RevisionId revision = session_(view).snapshot().revision();
    const DeleteGraphemeOutcome outcome = mode == DeleteGraphemeMode::Structural
                                              ? structural_delete(view, forward)
                                              : raw_delete(view, forward);
    if (session_(view).snapshot().revision() != revision) {
        notify_edited();
    }
    return outcome;
}

void EditingMechanisms::newline(ViewId view) {
    session_(view).enter();
    notify_edited();
}

std::optional<FormatRole> EditingMechanisms::indent(ViewId view) {
    EditSession& active = session_(view);
    if (!active.has_language_facet(LanguageFacet::Indentation)) {
        return std::nullopt;
    }
    const RevisionId revision = active.snapshot().revision();
    const TextOffset caret = active.caret();
    const IndentDecision decision = active.indent();
    if (active.snapshot().revision() != revision) {
        notify_edited();
    } else if (active.caret() != caret) {
        notify_caret_moved();
    }
    return decision.role;
}

void EditingMechanisms::type_text(ViewId view, std::string_view text) {
    EditSession& active = session_(view);
    if (!active.has_language_facet(LanguageFacet::StructuralEditing)) {
        throw std::logic_error("structural typing requires a structural-editing language facet");
    }
    const RevisionId revision = active.snapshot().revision();
    active.type_text(text);
    if (active.snapshot().revision() != revision) {
        notify_edited();
    }
}

DeleteGraphemeOutcome EditingMechanisms::structural_delete(ViewId view, bool forward) {
    EditSession& active = session_(view);
    if (!active.has_language_facet(LanguageFacet::StructuralEditing)) {
        throw std::logic_error("structural deletion requires a structural-editing language facet");
    }
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = active.caret();
    if ((forward && caret.value >= text.size_bytes()) || (!forward && caret.value == 0)) {
        return DeleteGraphemeOutcome::Unchanged;
    }
    const TextOffset target = forward ? caret : ui::previous_grapheme(text, caret);
    const char character = text.byte_at(target);
    const auto is_open = [](char ch) { return ch == '(' || ch == '[' || ch == '{'; };
    const auto is_close = [](char ch) { return ch == ')' || ch == ']' || ch == '}'; };
    if (is_open(character) || is_close(character)) {
        const std::optional<TextRange> pair =
            matching_bracket_range(active.analysis(LanguageFacet::StructuralEditing).tree, target);
        if (!pair) {
            return raw_delete(view, forward);
        }
        if (pair->length() == 2) {
            active.erase(*pair);
            return DeleteGraphemeOutcome::Deleted;
        }
        active.set_caret(forward ? TextOffset{target.value + 1} : target);
        notify_caret_moved();
        return DeleteGraphemeOutcome::MovedOverPair;
    }
    if (character == '"' || character == '\'') {
        const TokenBuffer& tokens = active.analysis(LanguageFacet::StructuralEditing).tree.tokens();
        std::size_t lo = 0;
        std::size_t hi = tokens.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (tokens[mid].range.end <= target) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < tokens.size()) {
            const Token token = tokens[lo];
            const bool literal = token.kind == TokenKind::StringLiteral ||
                                 token.kind == TokenKind::RawStringLiteral ||
                                 token.kind == TokenKind::CharacterLiteral;
            TextOffset opening_quote = token.range.start;
            while (opening_quote < token.range.end && text.byte_at(opening_quote) != character) {
                ++opening_quote.value;
            }
            const bool delimiter =
                target == opening_quote || target.value + 1 == token.range.end.value;
            if (!literal || has_flag(token.flags, LexicalFlags::Unterminated) || !delimiter) {
                return raw_delete(view, forward);
            }
        } else {
            return raw_delete(view, forward);
        }
        const char other =
            forward
                ? (target.value >= 1 ? text.byte_at(TextOffset{target.value - 1}) : '\0')
                : (target.value + 1 < text.size_bytes() ? text.byte_at(TextOffset{target.value + 1})
                                                        : '\0');
        if (other == character) {
            const std::uint32_t start = forward ? target.value - 1 : target.value;
            active.erase(make_range(start, start + 2));
            return DeleteGraphemeOutcome::Deleted;
        }
        active.set_caret(forward ? TextOffset{target.value + 1} : target);
        notify_caret_moved();
        return DeleteGraphemeOutcome::MovedOverLiteral;
    }
    return raw_delete(view, forward);
}

DeleteGraphemeOutcome EditingMechanisms::raw_delete(ViewId view, bool forward) {
    EditSession& active = session_(view);
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = active.caret();
    if ((forward && caret.value >= text.size_bytes()) || (!forward && caret.value == 0)) {
        return DeleteGraphemeOutcome::Unchanged;
    }
    active.erase(forward ? TextRange{caret, ui::next_grapheme(text, caret)}
                         : TextRange{ui::previous_grapheme(text, caret), caret});
    return DeleteGraphemeOutcome::Deleted;
}

void EditingMechanisms::notify_edited() {
    if (hooks_.edited) {
        hooks_.edited();
    }
}

void EditingMechanisms::notify_caret_moved() {
    if (hooks_.caret_moved) {
        hooks_.caret_moved();
    }
}

} // namespace cind
