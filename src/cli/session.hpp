#pragma once

#include "commands/editor_commands.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>

namespace cind {

// Shared execution core for the repl and the fixture runner: a document, a
// caret and a style, driven by editor commands. Caret history is keyed by
// undo-tree node, so undo/redo restore the caret on any branch.
class EditSession {
public:
    explicit EditSession(std::string initial_text, CppIndentStyle style = {});

    const DocumentSnapshot snapshot() const { return document_.snapshot(); }
    TextOffset caret() const { return caret_; }
    void set_caret(TextOffset caret);
    CppIndentStyle& style() { return style_; }
    const CppIndentStyle& style() const { return style_; }

    void type_text(std::string_view text);
    // One-transaction insert at the caret, bypassing the typed-char pipeline.
    // For multi-byte input (a UTF-8 code point is one undo unit) and pastes.
    void insert_text(std::string_view text);
    EnterResult enter();
    IndentDecision indent(); // reindent the caret's line
    // Erases a range as one undo unit; the caret settles at range.start.
    void erase(TextRange range);
    bool undo();
    bool redo();

    // Pure query: explain the indent of the caret's line.
    IndentDecision explain() const;

    // Analysis of the current revision (cached; incrementally maintained
    // across every command). Valid until the next mutation.
    const Analysis& analysis() const { return analyzer_.analyze(document_.snapshot()); }

    std::string render_with_caret() const;

private:
    struct CaretPair {
        TextOffset before; // caret before the edit that created the node
        TextOffset after;  // caret right after it
    };

    void record_caret(TextOffset before);
    void clamp_caret() { caret_.value = std::min(caret_.value, snapshot().size_bytes()); }

    Document document_;
    TextOffset caret_{0};
    CppIndentStyle style_;
    std::map<UndoNodeId, CaretPair> undo_carets_;
    mutable Analyzer analyzer_; // memo of a pure function — const-safe
};

// Parses "key: value" into a style field; returns false on unknown key or
// bad value. Keys mirror CppIndentStyle member names.
bool set_style_field(CppIndentStyle& style, std::string_view key, std::string_view value);

// Splits a '^' caret marker out of fixture text. Returns npos-equivalent
// (text size) as caret when no marker is present.
struct CaretText {
    std::string text;
    TextOffset caret;
    bool had_marker = false;
};
CaretText split_caret_marker(std::string_view marked);

} // namespace cind
