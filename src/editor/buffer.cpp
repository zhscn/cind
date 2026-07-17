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
    : id_(id), name_(std::move(spec.name)), kind_(spec.kind),
      resource_uri_(std::move(spec.resource_uri)), read_only_(spec.read_only),
      document_(std::move(spec.initial_text), document_id),
      save_point_(document_.snapshot().content()), settings_(settings, SettingScope::Buffer),
      modes_(id, modes) {}

bool Buffer::modified() const {
    return diff_edit(save_point_, document_.snapshot().content()).has_value();
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

std::string BufferRegistry::fallback_name(const BufferSpec& spec) {
    if (spec.resource_uri && !spec.resource_uri->empty()) {
        const std::filesystem::path path(*spec.resource_uri);
        if (!path.filename().empty()) {
            return path.filename().string();
        }
    }
    switch (spec.kind) {
    case BufferKind::File:
        return "untitled";
    case BufferKind::Scratch:
        return "*scratch*";
    case BufferKind::Generated:
        return "*generated*";
    case BufferKind::Process:
        return "*process*";
    case BufferKind::Minibuffer:
        return " *minibuffer*";
    }
    return "*buffer*";
}

std::string BufferRegistry::unique_name(std::string requested, std::optional<BufferId> self) const {
    if (requested.empty()) {
        requested = "*buffer*";
    }
    auto available = [&](const std::string& candidate) {
        auto it = by_name_.find(candidate);
        return it == by_name_.end() || (self && it->second == *self);
    };
    if (available(requested)) {
        return requested;
    }
    for (std::uint32_t suffix = 2;; ++suffix) {
        std::string candidate = std::format("{}<{}>", requested, suffix);
        if (available(candidate)) {
            return candidate;
        }
        if (suffix == std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("buffer name space is exhausted");
        }
    }
}

BufferId BufferRegistry::create(BufferSpec spec) {
    if (spec.resource_uri && by_resource_.contains(*spec.resource_uri)) {
        throw std::invalid_argument("a buffer already owns this resource");
    }
    if (spec.name.empty()) {
        spec.name = fallback_name(spec);
    }
    spec.name = unique_name(std::move(spec.name));

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
    by_name_.emplace(entry.value->name(), id);
    if (entry.value->resource_uri()) {
        by_resource_.emplace(*entry.value->resource_uri(), id);
    }
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
    by_name_.erase(buffer->name());
    if (buffer->resource_uri()) {
        by_resource_.erase(*buffer->resource_uri());
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

std::optional<BufferId> BufferRegistry::find_by_name(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    return it == by_name_.end() ? std::nullopt : std::optional<BufferId>(it->second);
}

std::optional<BufferId> BufferRegistry::find_by_resource(std::string_view uri) const {
    auto it = by_resource_.find(std::string(uri));
    return it == by_resource_.end() ? std::nullopt : std::optional<BufferId>(it->second);
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

void BufferRegistry::rename(BufferId id, std::string requested_name) {
    Buffer& buffer = get(id);
    std::string name = unique_name(std::move(requested_name), id);
    if (name == buffer.name_) {
        return;
    }
    by_name_.erase(buffer.name_);
    buffer.name_ = std::move(name);
    by_name_.emplace(buffer.name_, id);
}

void BufferRegistry::set_resource(BufferId id, std::optional<std::string> uri, BufferKind kind) {
    Buffer& buffer = get(id);
    if (uri) {
        auto it = by_resource_.find(*uri);
        if (it != by_resource_.end() && it->second != id) {
            throw std::invalid_argument("another buffer already owns this resource");
        }
    }
    if (buffer.resource_uri_) {
        by_resource_.erase(*buffer.resource_uri_);
    }
    buffer.resource_uri_ = std::move(uri);
    buffer.kind_ = kind;
    if (buffer.resource_uri_) {
        by_resource_.emplace(*buffer.resource_uri_, id);
    }
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

} // namespace cind
