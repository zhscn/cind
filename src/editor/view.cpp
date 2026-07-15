#include "editor/view.hpp"

#include <algorithm>
#include <stdexcept>

namespace cind {

ViewRegistry::~ViewRegistry() {
    for (Slot& slot : slots_) {
        if (slot.value) {
            remove_anchors(*slot.value);
            slot.value.reset();
        }
    }
}

AnchorId ViewRegistry::make_anchor(Buffer& buffer, TextOffset offset, AnchorAffinity affinity) {
    if (offset.value > buffer.snapshot().size_bytes()) {
        throw std::out_of_range("view position is outside the buffer");
    }
    return buffer.document_.create_anchor(offset, affinity);
}

ViewId ViewRegistry::create(BufferId buffer_id, TextOffset caret_offset) {
    Buffer& buffer = buffers_->get(buffer_id);
    const AnchorId caret = make_anchor(buffer, caret_offset, AnchorAffinity::AfterInsertion);

    std::optional<std::uint32_t> reserved_slot;
    try {
        std::uint32_t slot_index = 0;
        if (free_slots_.empty()) {
            if (slots_.size() >= ViewId::invalid_slot) {
                throw std::overflow_error("view registry is exhausted");
            }
            slot_index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back({});
        } else {
            slot_index = free_slots_.back();
            free_slots_.pop_back();
        }
        reserved_slot = slot_index;
        Slot& slot = slots_[slot_index];
        const ViewId id{slot_index, slot.generation};
        slot.value = std::unique_ptr<View>(new View(id, buffer_id, caret, *settings_));
        ++buffer.attached_views_;
        return id;
    } catch (...) {
        buffer.document_.remove_anchor(caret);
        if (reserved_slot) {
            free_slots_.push_back(*reserved_slot);
        }
        throw;
    }
}

void ViewRegistry::remove_anchors(View& view) {
    Buffer& buffer = buffers_->get(view.buffer_id_);
    if (view.mark_) {
        buffer.document_.remove_anchor(*view.mark_);
        view.mark_.reset();
    }
    buffer.document_.remove_anchor(view.caret_);
    --buffer.attached_views_;
}

bool ViewRegistry::erase(ViewId id) {
    View* view = try_get(id);
    if (view == nullptr) {
        return false;
    }
    remove_anchors(*view);
    Slot& slot = slots_[id.slot];
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
    free_slots_.push_back(id.slot);
    return true;
}

View* ViewRegistry::try_get(ViewId id) {
    if (!id.valid() || id.slot >= slots_.size()) {
        return nullptr;
    }
    Slot& slot = slots_[id.slot];
    return slot.generation == id.generation ? slot.value.get() : nullptr;
}

const View* ViewRegistry::try_get(ViewId id) const {
    return const_cast<ViewRegistry*>(this)->try_get(id);
}

View& ViewRegistry::get(ViewId id) {
    if (View* view = try_get(id)) {
        return *view;
    }
    throw std::out_of_range("unknown or stale view id");
}

const View& ViewRegistry::get(ViewId id) const {
    return const_cast<ViewRegistry*>(this)->get(id);
}

TextOffset ViewRegistry::caret(ViewId id) const {
    const View& view = get(id);
    return buffers_->get(view.buffer_id_).document_.anchor_offset(view.caret_);
}

std::optional<TextOffset> ViewRegistry::mark(ViewId id) const {
    const View& view = get(id);
    if (!view.mark_) {
        return std::nullopt;
    }
    return buffers_->get(view.buffer_id_).document_.anchor_offset(*view.mark_);
}

void ViewRegistry::set_caret(ViewId id, TextOffset caret_offset) {
    View& view = get(id);
    Buffer& buffer = buffers_->get(view.buffer_id_);
    const AnchorId replacement = make_anchor(buffer, caret_offset, AnchorAffinity::AfterInsertion);
    buffer.document_.remove_anchor(view.caret_);
    view.caret_ = replacement;
}

std::optional<TextRange> ViewRegistry::selection(ViewId id) const {
    const View& view = get(id);
    if (!view.mark_) {
        return std::nullopt;
    }
    const Buffer& buffer = buffers_->get(view.buffer_id_);
    const TextOffset anchor = buffer.document_.anchor_offset(*view.mark_);
    const TextOffset head = buffer.document_.anchor_offset(view.caret_);
    if (anchor == head) {
        return std::nullopt;
    }
    return anchor < head ? TextRange{anchor, head} : TextRange{head, anchor};
}

void ViewRegistry::set_selection(ViewId id, SelectionEndpoints selection) {
    View& view = get(id);
    Buffer& buffer = buffers_->get(view.buffer_id_);
    const AnchorId new_mark =
        make_anchor(buffer, selection.anchor, AnchorAffinity::BeforeInsertion);
    AnchorId new_caret = 0;
    try {
        new_caret = make_anchor(buffer, selection.head, AnchorAffinity::AfterInsertion);
    } catch (...) {
        buffer.document_.remove_anchor(new_mark);
        throw;
    }
    if (view.mark_) {
        buffer.document_.remove_anchor(*view.mark_);
    }
    buffer.document_.remove_anchor(view.caret_);
    view.mark_ = new_mark;
    view.caret_ = new_caret;
}

void ViewRegistry::clear_selection(ViewId id) {
    View& view = get(id);
    if (!view.mark_) {
        return;
    }
    Buffer& buffer = buffers_->get(view.buffer_id_);
    buffer.document_.remove_anchor(*view.mark_);
    view.mark_.reset();
}

} // namespace cind
