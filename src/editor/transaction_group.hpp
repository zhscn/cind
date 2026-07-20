#pragma once

#include "document/document.hpp"
#include "editor/ids.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cind {

using TransactionGroupId = std::uint64_t;
inline constexpr TransactionGroupId kInvalidTransactionGroup = 0;

struct TransactionGroupEntry {
    BufferId buffer;
    UndoNodeId before = kInvalidUndoNode;
    UndoNodeId after = kInvalidUndoNode;
};

struct TransactionGroup {
    TransactionGroupId id = kInvalidTransactionGroup;
    std::string source;
    std::vector<TransactionGroupEntry> entries;
    std::uint64_t created_at = 0;
};

struct TransactionGroupResult {
    TransactionGroupId group = kInvalidTransactionGroup;
    std::vector<BufferId> changed;
    std::vector<BufferId> skipped;
};

class TransactionGroupRegistry {
public:
    TransactionGroupId record(std::string source, std::vector<TransactionGroupEntry> entries);
    const TransactionGroup* find(TransactionGroupId group) const;
    std::span<const TransactionGroup> groups() const { return groups_; }

    using CurrentPosition = std::function<std::optional<UndoNodeId>(BufferId)>;
    using Navigate = std::function<bool(BufferId, UndoNodeId)>;
    std::optional<TransactionGroupResult>
    undo(TransactionGroupId group, const CurrentPosition& current, const Navigate& navigate);
    std::optional<TransactionGroupResult>
    redo(TransactionGroupId group, const CurrentPosition& current, const Navigate& navigate);

private:
    std::optional<TransactionGroupResult> move(TransactionGroupId group, bool redo,
                                               const CurrentPosition& current,
                                               const Navigate& navigate);

    std::vector<TransactionGroup> groups_;
    TransactionGroupId next_id_ = 0;
    std::uint64_t clock_ = 0;
};

} // namespace cind
