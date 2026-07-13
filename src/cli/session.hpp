#pragma once

#include "commands/editor_commands.hpp"

#include <string>
#include <string_view>

namespace cind {

// Shared execution core for the repl and the fixture runner: a document, a
// caret and a style, driven by editor commands. Caret history mirrors the
// document's undo stack so undo also restores the caret.
class EditSession {
public:
    explicit EditSession(std::string initial_text, CppIndentStyle style = {});

    const DocumentSnapshot snapshot() const { return document_.snapshot(); }
    TextOffset caret() const { return caret_; }
    void set_caret(TextOffset caret);
    CppIndentStyle& style() { return style_; }

    void type_text(std::string_view text);
    EnterResult enter();
    IndentDecision indent(); // reindent the caret's line
    bool undo();
    bool redo();

    // Pure query: explain the indent of the caret's line.
    IndentDecision explain() const;

    std::string render_with_caret() const;

private:
    Document document_;
    TextOffset caret_{0};
    CppIndentStyle style_;
    std::vector<TextOffset> undo_carets_;
    std::vector<TextOffset> redo_carets_;
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
