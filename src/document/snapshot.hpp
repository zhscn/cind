#pragma once

#include "document/text.hpp"
#include "document/text_types.hpp"

#include <string>

namespace cind {

// Immutable view of a document at one revision. O(1) to copy and to take;
// remains valid after later edits to the document (the Text value shares
// every untouched subtree with newer revisions).
class DocumentSnapshot {
public:
    DocumentId document_id() const { return document_id_; }
    RevisionId revision() const { return revision_; }

    // The text value at this revision; line and UTF-16 queries live on it.
    const Text& content() const { return text_; }
    std::string substring(TextRange range) const { return text_.substring(range); }
    std::uint32_t size_bytes() const { return text_.size_bytes(); }
    TextOffset end_offset() const { return text_.end_offset(); }

private:
    friend class Document;
    friend class EditTransaction;

    DocumentSnapshot(DocumentId document_id, RevisionId revision, Text text)
        : document_id_(document_id), revision_(revision), text_(std::move(text)) {}

    DocumentId document_id_ = 0;
    RevisionId revision_ = 0;
    Text text_;
};

} // namespace cind
