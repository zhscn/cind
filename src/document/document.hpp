#pragma once

#include "document/anchor.hpp"
#include "document/snapshot.hpp"
#include "document/text_types.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

class Document;

struct CommitResult {
    DocumentChange change;
    DocumentSnapshot snapshot;
};

// Collects edits against a document. Edit coordinates are *current pending*
// coordinates: each edit sees the text as already modified by earlier edits
// in this transaction. At most one transaction is active per document.
//
// The transaction maintains the pending edit list normalized (old-revision
// coordinates, ascending, non-overlapping) and settles anchor positions after
// every pending edit, so anchor_offset() is always current.
class EditTransaction {
public:
    EditTransaction(const EditTransaction&) = delete;
    EditTransaction& operator=(const EditTransaction&) = delete;
    EditTransaction(EditTransaction&& other) noexcept;
    EditTransaction& operator=(EditTransaction&&) = delete;
    ~EditTransaction();

    void replace(TextRange range, std::string_view replacement);
    void insert(TextOffset position, std::string_view text);
    void erase(TextRange range);

    // Anchor state as of the current pending text.
    TextOffset anchor_offset(AnchorId id) const;
    void set_anchor_affinity(AnchorId id, AnchorAffinity affinity);

    const Text& current_text() const { return text_; }
    // Snapshot of the pending state. Its revision is the revision commit()
    // would produce right now (base revision if no edits are pending).
    DocumentSnapshot speculative_snapshot() const;

    bool active() const { return document_ != nullptr; }
    CommitResult commit();
    void abort();

private:
    friend class Document;

    struct AnchorState {
        std::uint32_t offset;
        AnchorAffinity affinity;
    };

    explicit EditTransaction(Document& document);

    void require_active() const;
    void apply(std::uint32_t start, std::uint32_t end, std::string_view replacement);
    void finish();

    Document* document_ = nullptr;
    RevisionId base_revision_ = 0;
    bool record_undo_ = true;
    Text text_; // pending state
    std::vector<TextEdit> edits_;
    std::map<AnchorId, AnchorState> anchors_;
};

// The single authoritative text store. All mutation goes through transactions;
// every derived structure is keyed by RevisionId.
class Document {
public:
    // Newlines are normalized: "\r\n" and lone '\r' become '\n'.
    explicit Document(std::string text, DocumentId id = 0);

    DocumentId id() const { return id_; }
    RevisionId revision() const { return revision_; }
    DocumentSnapshot snapshot() const;

    EditTransaction begin_transaction();

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    // Undo/redo produce a *new* revision whose content equals the historical
    // text; revisions are strictly monotonic. Returns nullopt when empty.
    std::optional<DocumentChange> undo();
    std::optional<DocumentChange> redo();

    // Anchor management is not allowed while a transaction is active.
    AnchorId create_anchor(TextOffset offset, AnchorAffinity affinity);
    void remove_anchor(AnchorId id);
    TextOffset anchor_offset(AnchorId id) const;
    AnchorAffinity anchor_affinity(AnchorId id) const;
    void set_anchor_affinity(AnchorId id, AnchorAffinity affinity);

private:
    friend class EditTransaction;

    struct UndoEntry {
        std::vector<TextEdit> forward; // coordinates of the pre-change revision
        std::vector<TextEdit> inverse; // coordinates of the post-change revision
    };

    // Applies a normalized edit list (coordinates of the current revision)
    // without recording undo. Used by undo/redo.
    DocumentChange apply_edit_list(const std::vector<TextEdit>& edits);
    void require_no_transaction(const char* what) const;

    DocumentId id_;
    RevisionId revision_ = 0;
    Text text_;
    std::map<AnchorId, EditTransaction::AnchorState> anchors_;
    AnchorId next_anchor_id_ = 1;
    bool transaction_active_ = false;
    std::vector<UndoEntry> undo_stack_;
    std::vector<UndoEntry> redo_stack_;
};

} // namespace cind
