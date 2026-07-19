#include "editor/transaction_group.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cind {

TransactionGroupId TransactionGroupRegistry::record(std::string source,
                                                    std::vector<TransactionGroupEntry> entries) {
    if (source.empty() || entries.empty()) {
        throw std::invalid_argument("transaction group requires a source and entries");
    }
    std::ranges::sort(entries, {}, &TransactionGroupEntry::buffer);
    if (std::ranges::adjacent_find(entries, {}, &TransactionGroupEntry::buffer) != entries.end() ||
        std::ranges::any_of(entries, [](const TransactionGroupEntry& entry) {
            return !entry.buffer || entry.before == kInvalidUndoNode ||
                   entry.after == kInvalidUndoNode || entry.before == entry.after;
        })) {
        throw std::invalid_argument("transaction group entries are invalid");
    }
    if (next_id_ == std::numeric_limits<TransactionGroupId>::max()) {
        throw std::overflow_error("transaction group id space is exhausted");
    }
    const TransactionGroupId id = ++next_id_;
    groups_.push_back({.id = id,
                       .source = std::move(source),
                       .entries = std::move(entries),
                       .created_at = ++clock_,
                       .undone = false});
    return id;
}

const TransactionGroup* TransactionGroupRegistry::find(TransactionGroupId group) const {
    const auto found = std::ranges::find_if(
        groups_, [group](const TransactionGroup& candidate) { return candidate.id == group; });
    return found == groups_.end() ? nullptr : &*found;
}

std::optional<TransactionGroupResult> TransactionGroupRegistry::undo(TransactionGroupId group,
                                                                     const CurrentPosition& current,
                                                                     const Navigate& navigate) {
    return move(group, false, current, navigate);
}

std::optional<TransactionGroupResult> TransactionGroupRegistry::redo(TransactionGroupId group,
                                                                     const CurrentPosition& current,
                                                                     const Navigate& navigate) {
    return move(group, true, current, navigate);
}

std::optional<TransactionGroupResult> TransactionGroupRegistry::move(TransactionGroupId group,
                                                                     bool redo,
                                                                     const CurrentPosition& current,
                                                                     const Navigate& navigate) {
    auto found = std::ranges::find_if(
        groups_, [group](const TransactionGroup& candidate) { return candidate.id == group; });
    if (found == groups_.end() || found->undone != redo) {
        return std::nullopt;
    }
    TransactionGroupResult result{.group = group, .changed = {}, .skipped = {}};
    for (const TransactionGroupEntry& entry : found->entries) {
        const UndoNodeId expected = redo ? entry.before : entry.after;
        const UndoNodeId target = redo ? entry.after : entry.before;
        const std::optional<UndoNodeId> position = current(entry.buffer);
        if (!position || *position != expected || !navigate(entry.buffer, target)) {
            result.skipped.push_back(entry.buffer);
            continue;
        }
        result.changed.push_back(entry.buffer);
    }
    if (!result.changed.empty()) {
        found->undone = redo ? false : true;
    }
    return result;
}

} // namespace cind
