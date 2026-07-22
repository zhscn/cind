#include "editor/buffer.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cind {

Buffer::Buffer(BufferId id, DocumentId document_id, BufferSpec spec,
               const SettingRegistry& settings, ModeRegistry& modes)
    : id_(id), read_only_(spec.read_only),
      document_(std::move(spec.initial_text), document_id),
      save_point_(document_.snapshot().content()), settings_(settings, SettingScope::Buffer),
      modes_(id, modes) {}

bool Buffer::modified() const {
    return diff_edit(save_point_, document_.snapshot().content()).has_value();
}

void Buffer::mark_saved(Text content) {
    save_point_ = std::move(content);
    ++save_generation_;
}

const BufferLocation* Buffer::location_at(TextOffset offset) const {
    const std::vector<BufferLocation>& current = locations();
    const auto after =
        std::ranges::upper_bound(current, offset, {}, [](const BufferLocation& location) {
            return location.source_range.start;
        });
    if (after == current.begin()) {
        return nullptr;
    }
    const BufferLocation& candidate = *std::prev(after);
    return candidate.source_range.contains(offset) ||
                   (offset == snapshot().content().end_offset() &&
                    candidate.source_range.end == offset)
               ? &candidate
               : nullptr;
}

const std::vector<BufferLocation>& Buffer::locations() const {
    static const std::vector<BufferLocation> empty;
    return locations_revision_ == snapshot().revision() ? locations_ : empty;
}

std::vector<Diagnostic> Buffer::diagnostics() const {
    const RevisionId revision = snapshot().revision();
    std::vector<Diagnostic> result;
    for (const auto& [owner, set] : diagnostic_sets_) {
        (void)owner;
        if (set.revision == revision) {
            result.insert(result.end(), set.diagnostics.begin(), set.diagnostics.end());
        }
    }
    std::ranges::sort(result, [](const Diagnostic& left, const Diagnostic& right) {
        if (left.range.start != right.range.start) {
            return left.range.start < right.range.start;
        }
        return static_cast<std::uint8_t>(left.severity) < static_cast<std::uint8_t>(right.severity);
    });
    return result;
}

void Buffer::require_writable() const {
    if (read_only_) {
        throw std::logic_error("buffer is read-only");
    }
}

EditTransaction Buffer::begin_transaction() {
    require_writable();
    return document_.begin_transaction();
}

std::optional<DocumentChange> Buffer::undo() {
    require_writable();
    return document_.undo();
}

std::optional<DocumentChange> Buffer::redo() {
    require_writable();
    return document_.redo();
}

DocumentChange Buffer::undo_to(UndoNodeId position) {
    require_writable();
    return document_.undo_to(position);
}

AnchorId Buffer::create_navigation_anchor(TextOffset offset, AnchorAffinity affinity) {
    return document_.create_anchor(offset, affinity);
}

void Buffer::remove_navigation_anchor(AnchorId anchor) {
    document_.remove_anchor(anchor);
}

TextOffset Buffer::navigation_anchor_offset(AnchorId anchor) const {
    return document_.anchor_offset(anchor);
}

BufferId BufferRegistry::create(BufferSpec spec) {
    std::uint32_t slot = 0;
    if (free_slots_.empty()) {
        if (slots_.size() >= BufferId::invalid_slot) {
            throw std::overflow_error("buffer registry is exhausted");
        }
        slot = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    } else {
        slot = free_slots_.back();
        free_slots_.pop_back();
    }
    if (next_document_id_ == 0) {
        throw std::overflow_error("document id space is exhausted");
    }
    Slot& entry = slots_[slot];
    const BufferId id{slot, entry.generation};
    const DocumentId document_id = next_document_id_++;
    entry.value =
        std::unique_ptr<Buffer>(new Buffer(id, document_id, std::move(spec), *settings_, *modes_));
    return id;
}

bool BufferRegistry::erase(BufferId id) {
    Buffer* buffer = try_get(id);
    if (buffer == nullptr) {
        return false;
    }
    if (buffer->attached_view_count() != 0) {
        return false;
    }
    Slot& slot = slots_[id.slot];
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
    free_slots_.push_back(id.slot);
    return true;
}

Buffer* BufferRegistry::try_get(BufferId id) {
    if (!id.valid() || id.slot >= slots_.size()) {
        return nullptr;
    }
    Slot& slot = slots_[id.slot];
    return slot.generation == id.generation ? slot.value.get() : nullptr;
}

const Buffer* BufferRegistry::try_get(BufferId id) const {
    return const_cast<BufferRegistry*>(this)->try_get(id);
}

Buffer& BufferRegistry::get(BufferId id) {
    if (Buffer* buffer = try_get(id)) {
        return *buffer;
    }
    throw std::out_of_range("unknown or stale buffer id");
}

const Buffer& BufferRegistry::get(BufferId id) const {
    return const_cast<BufferRegistry*>(this)->get(id);
}

std::vector<BufferId> BufferRegistry::all() const {
    std::vector<BufferId> ids;
    ids.reserve(slots_.size() - free_slots_.size());
    for (std::uint32_t slot = 0; slot < slots_.size(); ++slot) {
        if (slots_[slot].value) {
            ids.push_back(BufferId{slot, slots_[slot].generation});
        }
    }
    return ids;
}

void BufferRegistry::set_locations(BufferId id, std::vector<BufferLocation> locations) {
    Buffer& buffer = get(id);
    const std::uint32_t document_size = buffer.snapshot().size_bytes();
    TextOffset previous_end{};
    bool first = true;
    for (const BufferLocation& location : locations) {
        if (location.source_range.empty() || location.source_range.end.value > document_size) {
            throw std::invalid_argument("buffer location has an invalid source range");
        }
        if (!first && location.source_range.start < previous_end) {
            throw std::invalid_argument("buffer locations must be ordered and non-overlapping");
        }
        if (location.resource.empty()) {
            throw std::invalid_argument("buffer location has no target resource");
        }
        previous_end = location.source_range.end;
        first = false;
    }
    buffer.locations_ = std::move(locations);
    buffer.locations_revision_ = buffer.snapshot().revision();
}

void BufferRegistry::set_diagnostics(BufferId id, std::string owner, RevisionId revision,
                                     std::vector<Diagnostic> diagnostics) {
    Buffer& buffer = get(id);
    if (owner.empty()) {
        throw std::invalid_argument("diagnostic owner must not be empty");
    }
    if (revision != buffer.snapshot().revision()) {
        throw std::invalid_argument("diagnostics do not match the current buffer revision");
    }
    const std::uint32_t size = buffer.snapshot().size_bytes();
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.range.start > diagnostic.range.end || diagnostic.range.end.value > size ||
            diagnostic.message.empty()) {
            throw std::invalid_argument("diagnostic has an invalid range or message");
        }
    }
    DiagnosticSet set{
        .owner = std::move(owner), .revision = revision, .diagnostics = std::move(diagnostics)};
    const std::string key = set.owner;
    buffer.diagnostic_sets_.insert_or_assign(key, std::move(set));
    ++buffer.diagnostics_generation_;
}

bool BufferRegistry::clear_diagnostics(BufferId id, std::string_view owner) {
    Buffer& buffer = get(id);
    if (buffer.diagnostic_sets_.erase(std::string(owner)) == 0) {
        return false;
    }
    ++buffer.diagnostics_generation_;
    return true;
}

} // namespace cind
