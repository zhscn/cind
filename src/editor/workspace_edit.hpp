#pragma once

#include "document/text_types.hpp"
#include "editor/buffer.hpp"
#include "editor/transaction_group.hpp"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace cind {

struct WorkspaceBufferEdit {
    BufferId buffer;
    RevisionId revision = 0;
    std::vector<TextEdit> edits;
};

// Applies edits expressed in immutable Buffer coordinates. Every participant
// is validated and every transaction is prepared before any Buffer publishes
// a new revision. The returned entries form one cross-Buffer undo group.
std::expected<std::vector<TransactionGroupEntry>, std::string>
apply_workspace_edit(BufferRegistry& buffers, std::span<const WorkspaceBufferEdit> edit);

} // namespace cind
