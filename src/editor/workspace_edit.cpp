#include "editor/workspace_edit.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

struct PreparedBufferEdit {
    BufferId buffer;
    RevisionId revision = 0;
    UndoNodeId before = kInvalidUndoNode;
    std::vector<TextEdit> edits;
};

struct PendingTransaction {
    PreparedBufferEdit* edit = nullptr;
    EditTransaction transaction;
};

std::expected<std::vector<PreparedBufferEdit>, std::string>
prepare_workspace_edit(BufferRegistry& buffers, std::span<const WorkspaceBufferEdit> source) {
    std::map<BufferId, PreparedBufferEdit> merged;
    for (const WorkspaceBufferEdit& requested : source) {
        Buffer* buffer = buffers.try_get(requested.buffer);
        if (buffer == nullptr) {
            return std::unexpected("workspace edit references an unavailable buffer");
        }
        const DocumentSnapshot snapshot = buffer->snapshot();
        if (requested.revision != snapshot.revision()) {
            return std::unexpected(std::format("workspace edit for '{}' is stale", buffer->name()));
        }
        if (buffer->read_only()) {
            return std::unexpected(
                std::format("workspace edit cannot modify read-only buffer '{}'", buffer->name()));
        }
        auto [found, inserted] = merged.try_emplace(
            requested.buffer,
            PreparedBufferEdit{.buffer = requested.buffer,
                               .revision = requested.revision,
                               .before = buffer->undo_position(),
                               .edits = {}});
        if (!inserted && found->second.revision != requested.revision) {
            return std::unexpected(
                std::format("workspace edit has conflicting revisions for '{}'", buffer->name()));
        }
        found->second.edits.insert(found->second.edits.end(), requested.edits.begin(),
                                   requested.edits.end());
    }

    std::vector<PreparedBufferEdit> prepared;
    prepared.reserve(merged.size());
    for (auto& [id, edit] : merged) {
        (void)id;
        Buffer& buffer = buffers.get(edit.buffer);
        const std::uint32_t size = buffer.snapshot().size_bytes();
        std::ranges::sort(edit.edits, {}, [](const TextEdit& value) {
            return std::pair(value.old_range.start, value.old_range.end);
        });
        TextOffset previous_end{};
        TextOffset previous_start{};
        bool previous_empty = false;
        bool first = true;
        for (const TextEdit& value : edit.edits) {
            if (value.old_range.start > value.old_range.end || value.old_range.end.value > size ||
                (!first && value.old_range.start < previous_end) ||
                (!first && previous_empty && value.old_range.empty() &&
                 value.old_range.start == previous_start)) {
                return std::unexpected(
                    std::format("workspace edit contains overlapping or invalid ranges for '{}'",
                                buffer.name()));
            }
            previous_start = value.old_range.start;
            previous_end = value.old_range.end;
            previous_empty = value.old_range.empty();
            first = false;
        }
        if (!edit.edits.empty()) {
            prepared.push_back(std::move(edit));
        }
    }
    return prepared;
}

} // namespace

std::expected<std::vector<TransactionGroupEntry>, std::string>
apply_workspace_edit(BufferRegistry& buffers, std::span<const WorkspaceBufferEdit> edit) {
    std::expected<std::vector<PreparedBufferEdit>, std::string> prepared =
        prepare_workspace_edit(buffers, edit);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }

    try {
        std::vector<PendingTransaction> pending;
        pending.reserve(prepared->size());
        for (PreparedBufferEdit& entry : *prepared) {
            pending.push_back({.edit = &entry,
                               .transaction = buffers.get(entry.buffer).begin_transaction()});
        }
        for (PendingTransaction& entry : pending) {
            for (auto current = entry.edit->edits.rbegin(); current != entry.edit->edits.rend();
                 ++current) {
                entry.transaction.replace(current->old_range, current->new_text);
            }
        }

        std::vector<TransactionGroupEntry> result;
        result.reserve(pending.size());
        try {
            for (PendingTransaction& entry : pending) {
                (void)entry.transaction.commit();
                Buffer& buffer = buffers.get(entry.edit->buffer);
                const UndoNodeId after = buffer.undo_position();
                if (entry.edit->before != after) {
                    result.push_back({.buffer = entry.edit->buffer,
                                      .before = entry.edit->before,
                                      .after = after});
                }
            }
        } catch (...) {
            for (auto committed = result.rbegin(); committed != result.rend(); ++committed) {
                try {
                    (void)buffers.get(committed->buffer).undo_to(committed->before);
                } catch (...) {
                    std::terminate();
                }
            }
            throw;
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected(std::format("cannot apply workspace edit: {}", exception.what()));
    } catch (...) {
        return std::unexpected("cannot apply workspace edit");
    }
}

} // namespace cind
