#include "document/document.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace cind {

namespace {

std::string normalize_newlines(std::string text) {
    if (text.find('\r') == std::string::npos) {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out.push_back('\n');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Settles one anchor across replace([start, end) -> new_len bytes), all in
// pending coordinates. An anchor inside a replaced range settles after the
// replacement text (so a caret inside reindented whitespace lands after the
// new indentation). Affinity only matters for a pure insertion exactly at
// the anchor.
std::uint32_t adjust_anchor(std::uint32_t offset, AnchorAffinity affinity, std::uint32_t start,
                            std::uint32_t end, std::uint32_t new_len) {
    if (end > start) {
        if (offset <= start) {
            return offset;
        }
        if (offset >= end) {
            return offset - (end - start) + new_len;
        }
        return start + new_len;
    }
    if (offset < start) {
        return offset;
    }
    if (offset > start) {
        return offset + new_len;
    }
    return affinity == AnchorAffinity::AfterInsertion ? offset + new_len : offset;
}

} // namespace

// ---------------------------------------------------------------- Document

Document::Document(std::string text, DocumentId id)
    : id_(id), text_(normalize_newlines(std::move(text))) {}

DocumentSnapshot Document::snapshot() const { return DocumentSnapshot(id_, revision_, text_); }

EditTransaction Document::begin_transaction() {
    if (transaction_active_) {
        throw std::logic_error("Document: a transaction is already active");
    }
    return EditTransaction(*this);
}

void Document::require_no_transaction(const char* what) const {
    if (transaction_active_) {
        throw std::logic_error(std::string("Document: ") + what +
                               " is not allowed during an active transaction");
    }
}

DocumentChange Document::apply_edit_list(const std::vector<TextEdit>& edits) {
    EditTransaction tx = begin_transaction();
    tx.record_undo_ = false;
    // Back to front, so each edit's coordinates are unaffected by the others.
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        tx.replace(it->old_range, it->new_text);
    }
    return tx.commit().change;
}

std::optional<DocumentChange> Document::undo() {
    require_no_transaction("undo");
    if (undo_stack_.empty()) {
        return std::nullopt;
    }
    UndoEntry entry = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    DocumentChange change = apply_edit_list(entry.inverse);
    redo_stack_.push_back(std::move(entry));
    return change;
}

std::optional<DocumentChange> Document::redo() {
    require_no_transaction("redo");
    if (redo_stack_.empty()) {
        return std::nullopt;
    }
    UndoEntry entry = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    DocumentChange change = apply_edit_list(entry.forward);
    undo_stack_.push_back(std::move(entry));
    return change;
}

AnchorId Document::create_anchor(TextOffset offset, AnchorAffinity affinity) {
    require_no_transaction("create_anchor");
    if (offset.value > text_.size_bytes()) {
        throw std::out_of_range("Document: anchor offset out of range");
    }
    AnchorId id = next_anchor_id_++;
    anchors_.emplace(id, EditTransaction::AnchorState{offset.value, affinity});
    return id;
}

void Document::remove_anchor(AnchorId id) {
    require_no_transaction("remove_anchor");
    if (anchors_.erase(id) == 0) {
        throw std::out_of_range("Document: unknown anchor");
    }
}

TextOffset Document::anchor_offset(AnchorId id) const {
    auto it = anchors_.find(id);
    if (it == anchors_.end()) {
        throw std::out_of_range("Document: unknown anchor");
    }
    return TextOffset{it->second.offset};
}

AnchorAffinity Document::anchor_affinity(AnchorId id) const {
    auto it = anchors_.find(id);
    if (it == anchors_.end()) {
        throw std::out_of_range("Document: unknown anchor");
    }
    return it->second.affinity;
}

void Document::set_anchor_affinity(AnchorId id, AnchorAffinity affinity) {
    require_no_transaction("set_anchor_affinity");
    auto it = anchors_.find(id);
    if (it == anchors_.end()) {
        throw std::out_of_range("Document: unknown anchor");
    }
    it->second.affinity = affinity;
}

// --------------------------------------------------------- EditTransaction

EditTransaction::EditTransaction(Document& document)
    : document_(&document), base_revision_(document.revision_), text_(document.text_),
      anchors_(document.anchors_) {
    document.transaction_active_ = true;
}

EditTransaction::EditTransaction(EditTransaction&& other) noexcept
    : document_(other.document_), base_revision_(other.base_revision_),
      record_undo_(other.record_undo_), text_(std::move(other.text_)),
      edits_(std::move(other.edits_)), anchors_(std::move(other.anchors_)) {
    other.document_ = nullptr;
}

EditTransaction::~EditTransaction() {
    if (document_ != nullptr) {
        abort();
    }
}

void EditTransaction::require_active() const {
    if (document_ == nullptr) {
        throw std::logic_error("EditTransaction: not active");
    }
}

void EditTransaction::replace(TextRange range, std::string_view replacement) {
    require_active();
    apply(range.start.value, range.end.value, replacement);
}

void EditTransaction::insert(TextOffset position, std::string_view text) {
    require_active();
    apply(position.value, position.value, text);
}

void EditTransaction::erase(TextRange range) {
    require_active();
    apply(range.start.value, range.end.value, {});
}

// Folds an edit given in pending coordinates [cs, ce) into `edits_`, which is
// kept normalized in base-revision coordinates (ascending, non-overlapping).
// Existing edits whose pending-coordinate extent overlaps or touches [cs, ce]
// are merged with the new edit into a single record.
void EditTransaction::apply(std::uint32_t cs, std::uint32_t ce, std::string_view replacement) {
    if (cs > ce || ce > text_.size_bytes()) {
        throw std::out_of_range("EditTransaction: edit out of range");
    }
    if (replacement.contains('\r')) {
        throw std::invalid_argument("EditTransaction: '\\r' not allowed; newlines must be '\\n'");
    }

    const auto new_len = static_cast<std::uint32_t>(replacement.size());

    std::int64_t delta = 0; // running sum of new_len - old_len over edits_
    std::optional<std::size_t> first;
    std::optional<std::size_t> last;
    std::int64_t delta_before_first = 0;
    std::int64_t delta_after_last = 0;
    std::uint32_t first_cur_start = 0;
    std::uint32_t last_cur_end = 0;
    std::size_t insert_pos = edits_.size();
    std::int64_t delta_at_insert = 0;
    bool insert_pos_found = false;

    for (std::size_t i = 0; i < edits_.size(); ++i) {
        const std::uint32_t old_len = edits_[i].old_range.length();
        const auto edit_new_len = static_cast<std::uint32_t>(edits_[i].new_text.size());
        const auto cur_start = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(edits_[i].old_range.start.value) + delta);
        const std::uint32_t cur_end = cur_start + edit_new_len;

        if (cur_end >= cs && cur_start <= ce) {
            if (!first) {
                first = i;
                delta_before_first = delta;
                first_cur_start = cur_start;
            }
            last = i;
            last_cur_end = cur_end;
            delta_after_last = delta + static_cast<std::int64_t>(edit_new_len) - old_len;
        } else if (!insert_pos_found && cur_start > ce) {
            insert_pos = i;
            delta_at_insert = delta;
            insert_pos_found = true;
        }
        delta += static_cast<std::int64_t>(edit_new_len) - old_len;
    }
    if (!insert_pos_found) {
        delta_at_insert = delta;
    }

    if (!first) {
        const auto old_start =
            static_cast<std::uint32_t>(static_cast<std::int64_t>(cs) - delta_at_insert);
        const std::uint32_t old_end = old_start + (ce - cs);
        edits_.insert(edits_.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                      TextEdit{make_range(old_start, old_end), std::string(replacement)});
    } else {
        const std::uint32_t mcs = std::min(cs, first_cur_start);
        const std::uint32_t mce = std::max(ce, last_cur_end);
        std::string merged;
        merged.reserve((cs - mcs) + replacement.size() + (mce - ce));
        merged.append(text_.substring(make_range(mcs, cs)));
        merged.append(replacement);
        merged.append(text_.substring(make_range(ce, mce)));
        const std::uint32_t old_start =
            mcs < first_cur_start
                ? static_cast<std::uint32_t>(static_cast<std::int64_t>(mcs) - delta_before_first)
                : edits_[*first].old_range.start.value;
        const std::uint32_t old_end =
            mce > last_cur_end
                ? static_cast<std::uint32_t>(static_cast<std::int64_t>(mce) - delta_after_last)
                : edits_[*last].old_range.end.value;
        edits_.erase(edits_.begin() + static_cast<std::ptrdiff_t>(*first),
                     edits_.begin() + static_cast<std::ptrdiff_t>(*last) + 1);
        edits_.insert(edits_.begin() + static_cast<std::ptrdiff_t>(*first),
                      TextEdit{make_range(old_start, old_end), std::move(merged)});
    }

    text_ = text_.replace(make_range(cs, ce), replacement);

    for (auto& [id, state] : anchors_) {
        state.offset = adjust_anchor(state.offset, state.affinity, cs, ce, new_len);
    }
}

TextOffset EditTransaction::anchor_offset(AnchorId id) const {
    require_active();
    auto it = anchors_.find(id);
    if (it == anchors_.end()) {
        throw std::out_of_range("EditTransaction: unknown anchor");
    }
    return TextOffset{it->second.offset};
}

void EditTransaction::set_anchor_affinity(AnchorId id, AnchorAffinity affinity) {
    require_active();
    auto it = anchors_.find(id);
    if (it == anchors_.end()) {
        throw std::out_of_range("EditTransaction: unknown anchor");
    }
    it->second.affinity = affinity;
}

DocumentSnapshot EditTransaction::speculative_snapshot() const {
    require_active();
    RevisionId revision = edits_.empty() ? base_revision_ : base_revision_ + 1;
    return DocumentSnapshot(document_->id_, revision, text_);
}

CommitResult EditTransaction::commit() {
    require_active();
    Document& doc = *document_;

    if (edits_.empty()) {
        DocumentChange change;
        change.old_revision = base_revision_;
        change.new_revision = base_revision_;
        CommitResult result{std::move(change), doc.snapshot()};
        finish();
        return result;
    }

    const Text& old_text = doc.text_;

    std::vector<TextEdit> inverse;
    inverse.reserve(edits_.size());
    std::int64_t delta = 0;
    for (const TextEdit& edit : edits_) {
        const auto new_start = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(edit.old_range.start.value) + delta);
        const auto new_end = new_start + static_cast<std::uint32_t>(edit.new_text.size());
        inverse.push_back(
            TextEdit{make_range(new_start, new_end), old_text.substring(edit.old_range)});
        delta += static_cast<std::int64_t>(edit.new_text.size()) - edit.old_range.length();
    }

    DocumentChange change;
    change.old_revision = base_revision_;
    change.new_revision = base_revision_ + 1;
    change.edits = edits_;
    change.affected_old_range =
        TextRange{edits_.front().old_range.start, edits_.back().old_range.end};
    change.affected_new_range =
        TextRange{inverse.front().old_range.start, inverse.back().old_range.end};

    doc.text_ = std::move(text_);
    doc.revision_ = base_revision_ + 1;
    doc.anchors_ = std::move(anchors_);

    if (record_undo_) {
        doc.undo_stack_.push_back(Document::UndoEntry{std::move(edits_), std::move(inverse)});
        doc.redo_stack_.clear();
    }

    CommitResult result{std::move(change), doc.snapshot()};
    finish();
    return result;
}

void EditTransaction::abort() {
    require_active();
    finish();
}

void EditTransaction::finish() {
    document_->transaction_active_ = false;
    document_ = nullptr;
}

} // namespace cind
