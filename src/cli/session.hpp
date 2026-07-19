#pragma once

#include "commands/editor_commands.hpp"
#include "editor/language_mechanism.hpp"
#include "editor/runtime.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cind {

// Shared execution core for the repl and the fixture runner: a document, a
// caret and a style, driven by editor commands. Caret history is keyed by
// undo-tree node, so undo/redo restore the caret on any branch.
class EditSession {
public:
    explicit EditSession(std::string initial_text, CppIndentStyle style = {});
    EditSession(EditorRuntime& runtime, BufferId buffer, ViewId view, CppIndentStyle style = {});
    EditSession(EditorRuntime& runtime, BufferId buffer, ViewId view,
                std::shared_ptr<CppIndentStyle> style);

    const DocumentSnapshot snapshot() const { return buffer().snapshot(); }
    TextOffset caret() const { return runtime_->views().caret(view_id_); }
    void set_caret(TextOffset caret);
    std::optional<TextRange> selection() const { return runtime_->views().selection(view_id_); }
    ViewSelection selection_model() const { return runtime_->views().selection_model(view_id_); }
    std::optional<ViewSelection> active_selection() const {
        return runtime_->views().active_selection(view_id_);
    }
    std::vector<TextRange> selected_ranges() const {
        std::vector<TextRange> result;
        const std::optional<ViewSelection> active = active_selection();
        if (!active) {
            return result;
        }
        result.reserve(active->ranges.size());
        for (const SelectionRange& range : active->ranges) {
            if (const TextRange ordered = range.ordered(); !ordered.empty()) {
                result.push_back(ordered);
            }
        }
        return result;
    }
    std::optional<TextOffset> mark() const { return runtime_->views().mark(view_id_); }
    void set_selection(SelectionEndpoints selection) {
        runtime_->views().set_selection(view_id_, selection);
    }
    void set_selection(ViewSelection selection) {
        runtime_->views().set_selection(view_id_, std::move(selection));
    }
    void clear_selection() { runtime_->views().clear_selection(view_id_); }
    BufferId buffer_id() const { return buffer_id_; }
    ViewId view_id() const { return view_id_; }
    Buffer& buffer() { return runtime_->buffers().get(buffer_id_); }
    const Buffer& buffer() const { return runtime_->buffers().get(buffer_id_); }
    View& view() { return runtime_->views().get(view_id_); }
    const View& view() const { return runtime_->views().get(view_id_); }
    CppIndentStyle& style() { return *style_; }
    const CppIndentStyle& style() const { return *style_; }
    bool has_language_facet(LanguageFacet facet) const {
        return runtime_->language_provider(buffer_id_, facet).has_value();
    }

    void type_text(std::string_view text);
    // One-transaction insert at the caret, bypassing the typed-char pipeline.
    // For multi-byte input (a UTF-8 code point is one undo unit) and pastes.
    void insert_text(std::string_view text);
    // Inserts at every selection head in one transaction. One replacement is
    // broadcast to all heads; otherwise the vector is positional by range.
    void insert_text(std::span<const std::string> replacements);
    EnterResult enter();
    // Reindent the caret's line. On a blank line, the caret settles at the
    // resulting indentation even when the document text is already correct.
    IndentDecision indent();
    // Erases a range as one undo unit; the caret settles at range.start.
    void erase(TextRange range);
    // Replaces every range in one transaction. Replacements are positional:
    // one string per range. The returned selection contains the collapsed
    // post-edit positions and retains the input primary index and metadata.
    ViewSelection replace_selection(ViewSelection selection,
                                    std::span<const std::string> replacements);
    // Extracts one text value per range using the same granularity rules as
    // replace_selection.
    std::vector<std::string> selection_texts(const ViewSelection& selection) const;
    bool undo();
    bool redo();
    UndoNodeId undo_position() const { return buffer().undo_position(); }
    bool undo_to(UndoNodeId position);

    // Pure query: explain the indent of the caret's line.
    IndentDecision explain() const;

    // Analysis from the exact provider selected for `facet` by the Buffer's
    // scripted language profile. Valid until the next mutation.
    const Analysis& analysis(LanguageFacet facet = LanguageFacet::Syntax) const;
    std::optional<TextOffset> move_structurally(TextOffset from, StructuralMotion motion) const;
    std::expected<void, std::string> edit_structure(StructuralEdit edit);

    std::string render_with_caret() const;

private:
    struct CaretPair {
        TextOffset before; // caret before the edit that created the node
        TextOffset after;  // caret right after it
    };

    void record_caret(TextOffset before);
    Document& mutable_document();
    void clamp_caret();
    LanguageMechanismSession& language_session(LanguageFacet facet) const;
    void apply_language_change(const DocumentChange& change, const DocumentSnapshot& snapshot,
                               const LanguageMechanismSession* already_advanced = nullptr);

    struct MechanismState {
        std::shared_ptr<const LanguageMechanism> mechanism;
        std::unique_ptr<LanguageMechanismSession> session;
    };

    std::unique_ptr<EditorRuntime> owned_runtime_;
    EditorRuntime* runtime_ = nullptr;
    BufferId buffer_id_;
    ViewId view_id_;
    std::shared_ptr<CppIndentStyle> style_;
    std::map<UndoNodeId, CaretPair> undo_carets_;
    mutable std::vector<MechanismState> language_sessions_;
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
