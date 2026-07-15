#pragma once

#include "commands/editor_commands.hpp"
#include "editor/runtime.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace cind {

// Shared execution core for the repl and the fixture runner: a document, a
// caret and a style, driven by editor commands. Caret history is keyed by
// undo-tree node, so undo/redo restore the caret on any branch.
class EditSession {
public:
    explicit EditSession(std::string initial_text, CppIndentStyle style = {});
    EditSession(EditorRuntime& runtime, BufferId buffer, ViewId view, CppIndentStyle style = {});

    const DocumentSnapshot snapshot() const { return buffer().snapshot(); }
    TextOffset caret() const { return runtime_->views().caret(view_id_); }
    void set_caret(TextOffset caret);
    std::optional<TextRange> selection() const { return runtime_->views().selection(view_id_); }
    std::optional<TextOffset> mark() const { return runtime_->views().mark(view_id_); }
    void set_selection(SelectionEndpoints selection) {
        runtime_->views().set_selection(view_id_, selection);
    }
    void clear_selection() { runtime_->views().clear_selection(view_id_); }
    BufferId buffer_id() const { return buffer_id_; }
    ViewId view_id() const { return view_id_; }
    Buffer& buffer() { return runtime_->buffers().get(buffer_id_); }
    const Buffer& buffer() const { return runtime_->buffers().get(buffer_id_); }
    View& view() { return runtime_->views().get(view_id_); }
    const View& view() const { return runtime_->views().get(view_id_); }
    CppIndentStyle& style() { return style_; }
    const CppIndentStyle& style() const { return style_; }

    void type_text(std::string_view text);
    // One-transaction insert at the caret, bypassing the typed-char pipeline.
    // For multi-byte input (a UTF-8 code point is one undo unit) and pastes.
    void insert_text(std::string_view text);
    EnterResult enter();
    // Reindent the caret's line. On a blank line, the caret settles at the
    // resulting indentation even when the document text is already correct.
    IndentDecision indent();
    // Erases a range as one undo unit; the caret settles at range.start.
    void erase(TextRange range);
    bool undo();
    bool redo();

    // Pure query: explain the indent of the caret's line.
    IndentDecision explain() const;

    // Analysis of the current revision (cached; incrementally maintained
    // across every command). Valid until the next mutation.
    const Analysis& analysis() const { return analyzer_.analyze(snapshot()); }

    std::string render_with_caret() const;

private:
    struct CaretPair {
        TextOffset before; // caret before the edit that created the node
        TextOffset after;  // caret right after it
    };

    void record_caret(TextOffset before);
    Document& mutable_document();
    void clamp_caret();

    std::unique_ptr<EditorRuntime> owned_runtime_;
    EditorRuntime* runtime_ = nullptr;
    BufferId buffer_id_;
    ViewId view_id_;
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
