#pragma once

#include "document/line_index.hpp"
#include "document/text_types.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace cind {

// Immutable view of a document at one revision. Cheap to copy; remains valid
// after later edits to the document.
class DocumentSnapshot {
public:
    DocumentId document_id() const { return document_id_; }
    RevisionId revision() const { return revision_; }

    std::string_view text() const { return *text_; }
    std::string_view text(TextRange range) const {
        return std::string_view(*text_).substr(range.start.value, range.length());
    }
    std::uint32_t size_bytes() const { return static_cast<std::uint32_t>(text_->size()); }
    TextOffset end_offset() const { return TextOffset{size_bytes()}; }
    const LineIndex& lines() const { return *lines_; }

private:
    friend class Document;
    friend class EditTransaction;

    DocumentSnapshot(DocumentId document_id, RevisionId revision,
                     std::shared_ptr<const std::string> text,
                     std::shared_ptr<const LineIndex> lines)
        : document_id_(document_id), revision_(revision), text_(std::move(text)),
          lines_(std::move(lines)) {}

    DocumentId document_id_ = 0;
    RevisionId revision_ = 0;
    std::shared_ptr<const std::string> text_;
    std::shared_ptr<const LineIndex> lines_;
};

} // namespace cind
