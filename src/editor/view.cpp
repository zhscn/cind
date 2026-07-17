#include "editor/view.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cind {

ViewRegistry::ViewRegistry(BufferRegistry& buffers, const SettingRegistry& settings,
                           InputStateRegistry& input_states,
                           InputStrategyRegistry& input_strategies, ModeRegistry& modes)
    : buffers_(&buffers), settings_(&settings), input_states_(&input_states),
      input_strategies_(&input_strategies), modes_(&modes) {
    mode_listener_ = modes_->subscribe(
        [this](const BufferModePolicyChange& change) { refresh_mode_input_states(change.buffer); });
}

ViewRegistry::~ViewRegistry() {
    if (mode_listener_ != 0) {
        (void)modes_->unsubscribe(mode_listener_);
    }
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
        if (const std::optional<InputStateId> state = effective_base_state(*slot.value)) {
            slot.value->input_states_.set_base(*input_states_, id, *state);
        }
        return id;
    } catch (...) {
        if (reserved_slot) {
            Slot& slot = slots_[*reserved_slot];
            if (slot.value) {
                remove_anchors(*slot.value);
                slot.value.reset();
                ++slot.generation;
                if (slot.generation == 0) {
                    ++slot.generation;
                }
            } else {
                buffer.document_.remove_anchor(caret);
            }
            free_slots_.push_back(*reserved_slot);
        } else {
            buffer.document_.remove_anchor(caret);
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
    if (view->attached_windows_ != 0) {
        return false;
    }
    view->input_states_.reset(*input_states_, id);
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

void ViewRegistry::set_input_strategy(ViewId view_id, std::optional<InputStrategyId> strategy) {
    if (strategy) {
        (void)input_strategies_->definition(*strategy);
    }
    View& view = get(view_id);
    view.input_strategy_ = strategy;
    refresh_mode_input_state(view);
}

void ViewRegistry::set_base_input_state(ViewId view, InputStateId state) {
    get(view).input_states_.set_base(*input_states_, view, state);
}

void ViewRegistry::push_input_state(ViewId view, InputStateId state) {
    get(view).input_states_.push(*input_states_, view, state);
}

std::optional<InputStateId> ViewRegistry::pop_input_state(ViewId view) {
    return get(view).input_states_.pop(*input_states_, view);
}

void ViewRegistry::reset_input_states(ViewId view) {
    get(view).input_states_.reset(*input_states_, view);
}

void ViewRegistry::set_input_feedback(ViewId view, InputFeedback feedback) {
    get(view).input_states_.set_feedback(std::move(feedback));
}

void ViewRegistry::clear_input_feedback(ViewId view) {
    get(view).input_states_.clear_feedback();
}

std::optional<InputStateId> ViewRegistry::effective_base_state(const View& view) const {
    const EffectiveModePolicy policy =
        modes_->effective_policy(buffers_->get(view.buffer_id()).modes());
    if (policy.initial_state) {
        return policy.initial_state;
    }
    const std::optional<InputStrategyId> strategy =
        view.input_strategy_ ? view.input_strategy_ : input_strategies_->default_strategy();
    if (!strategy) {
        return std::nullopt;
    }
    return input_strategies_->state(*strategy, policy.interaction_class);
}

void ViewRegistry::refresh_mode_input_state(View& view) {
    if (const std::optional<InputStateId> state = effective_base_state(view)) {
        view.input_states_.set_base(*input_states_, view.id(), *state);
    }
}

void ViewRegistry::refresh_mode_input_states(std::optional<BufferId> buffer) {
    for (Slot& slot : slots_) {
        if (!slot.value || (buffer && slot.value->buffer_id() != *buffer)) {
            continue;
        }
        refresh_mode_input_state(*slot.value);
    }
}

} // namespace cind
