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
    mode_listener_ = modes_->subscribe([this](const BufferModePolicyChange& change) {
        if (change.before.interaction_class != change.after.interaction_class ||
            change.before.initial_state != change.after.initial_state) {
            refresh_mode_input_states(change.buffer);
        }
    });
}

ViewRegistry::~ViewRegistry() {
    if (mode_listener_ != 0) {
        (void)modes_->unsubscribe(mode_listener_);
    }
    for (Slot& slot : slots_) {
        if (slot.value) {
            slot.value->input_states_.release(*input_states_, slot.value->id());
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

View::AnchoredSelection ViewRegistry::anchor_selection(Buffer& buffer, ViewSelection selection) {
    if (selection.ranges.empty()) {
        throw std::invalid_argument("view selection requires at least one range");
    }
    if (selection.primary >= selection.ranges.size()) {
        throw std::out_of_range("view selection primary range is outside the range list");
    }

    View::AnchoredSelection result{
        .ranges = {}, .primary = selection.primary, .metadata = std::move(selection.metadata)};
    result.ranges.reserve(selection.ranges.size());
    try {
        for (const SelectionRange& range : selection.ranges) {
            const AnchorId anchor =
                make_anchor(buffer, range.anchor, AnchorAffinity::BeforeInsertion);
            AnchorId head = 0;
            try {
                head = make_anchor(buffer, range.head, AnchorAffinity::AfterInsertion);
                result.ranges.push_back(
                    {.anchor = anchor, .head = head, .granularity = range.granularity});
            } catch (...) {
                buffer.document_.remove_anchor(anchor);
                if (head != 0) {
                    buffer.document_.remove_anchor(head);
                }
                throw;
            }
        }
    } catch (...) {
        remove_anchored_selection(buffer, result);
        throw;
    }
    return result;
}

ViewSelection ViewRegistry::resolve_selection(const Buffer& buffer,
                                              const View::AnchoredSelection& selection) const {
    ViewSelection result{
        .ranges = {}, .primary = selection.primary, .metadata = selection.metadata};
    result.ranges.reserve(selection.ranges.size());
    for (const View::AnchoredSelectionRange& range : selection.ranges) {
        result.ranges.push_back({.anchor = buffer.document_.anchor_offset(range.anchor),
                                 .head = buffer.document_.anchor_offset(range.head),
                                 .granularity = range.granularity});
    }
    return result;
}

void ViewRegistry::remove_anchored_selection(Buffer& buffer, View::AnchoredSelection& selection,
                                             AnchorId retained_head) {
    for (const View::AnchoredSelectionRange& range : selection.ranges) {
        buffer.document_.remove_anchor(range.anchor);
        if (range.head != retained_head) {
            buffer.document_.remove_anchor(range.head);
        }
    }
    selection.ranges.clear();
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
    if (view.selection_) {
        remove_selection_anchors(view);
    } else {
        buffers_->get(view.buffer_id_).document_.remove_anchor(view.caret_);
    }
    clear_selection_history(view);
    --buffers_->get(view.buffer_id_).attached_views_;
}

void ViewRegistry::remove_selection_anchors(View& view, AnchorId retained_head) {
    if (!view.selection_) {
        return;
    }
    Buffer& buffer = buffers_->get(view.buffer_id_);
    remove_anchored_selection(buffer, *view.selection_, retained_head);
    view.selection_.reset();
}

bool ViewRegistry::erase(ViewId id) {
    View* view = try_get(id);
    if (view == nullptr) {
        return false;
    }
    if (view->attached_windows_ != 0) {
        return false;
    }
    view->input_states_.release(*input_states_, id);
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
    if (!view.selection_) {
        return std::nullopt;
    }
    const View::AnchoredSelectionRange& primary = view.selection_->ranges[view.selection_->primary];
    return buffers_->get(view.buffer_id_).document_.anchor_offset(primary.anchor);
}

void ViewRegistry::set_caret(ViewId id, TextOffset caret_offset) {
    View& view = get(id);
    Buffer& buffer = buffers_->get(view.buffer_id_);
    const AnchorId replacement = make_anchor(buffer, caret_offset, AnchorAffinity::AfterInsertion);
    buffer.document_.remove_anchor(view.caret_);
    view.caret_ = replacement;
    if (view.selection_) {
        view.selection_->ranges[view.selection_->primary].head = replacement;
    }
}

std::optional<TextRange> ViewRegistry::selection(ViewId id) const {
    const std::optional<ViewSelection> active = active_selection(id);
    if (!active) {
        return std::nullopt;
    }
    const TextRange primary = active->ranges[active->primary].ordered();
    if (primary.empty()) {
        return std::nullopt;
    }
    return primary;
}

ViewSelection ViewRegistry::selection_model(ViewId id) const {
    if (std::optional<ViewSelection> active = active_selection(id)) {
        return std::move(*active);
    }
    const TextOffset position = caret(id);
    return {.ranges = {{.anchor = position,
                        .head = position,
                        .granularity = SelectionGranularity::Character}},
            .primary = 0,
            .metadata = "()"};
}

std::optional<ViewSelection> ViewRegistry::active_selection(ViewId id) const {
    const View& view = get(id);
    if (!view.selection_) {
        return std::nullopt;
    }
    return resolve_selection(buffers_->get(view.buffer_id_), *view.selection_);
}

void ViewRegistry::set_selection(ViewId id, SelectionEndpoints selection) {
    set_selection(id, ViewSelection{.ranges = {selection}, .primary = 0, .metadata = "()"});
}

void ViewRegistry::set_selection(ViewId id, ViewSelection selection) {
    View& view = get(id);
    Buffer& buffer = buffers_->get(view.buffer_id_);
    View::AnchoredSelection replacement = anchor_selection(buffer, std::move(selection));

    if (view.selection_) {
        remove_selection_anchors(view);
    } else {
        buffer.document_.remove_anchor(view.caret_);
    }
    view.caret_ = replacement.ranges[replacement.primary].head;
    view.selection_ = std::move(replacement);
}

void ViewRegistry::clear_selection(ViewId id) {
    View& view = get(id);
    if (!view.selection_) {
        return;
    }
    const AnchorId retained_head = view.selection_->ranges[view.selection_->primary].head;
    remove_selection_anchors(view, retained_head);
    view.caret_ = retained_head;
}

void ViewRegistry::push_selection_history(ViewId id, ViewSelection selection) {
    View& view = get(id);
    Buffer& buffer = buffers_->get(view.buffer_id_);
    View::AnchoredSelection stored = anchor_selection(buffer, std::move(selection));
    try {
        view.selection_history_.push_back(std::move(stored));
    } catch (...) {
        remove_anchored_selection(buffer, stored);
        throw;
    }
}

std::optional<ViewSelection> ViewRegistry::pop_selection_history(ViewId id) {
    View& view = get(id);
    if (view.selection_history_.empty()) {
        return std::nullopt;
    }
    Buffer& buffer = buffers_->get(view.buffer_id_);
    View::AnchoredSelection& stored = view.selection_history_.back();
    ViewSelection result = resolve_selection(buffer, stored);
    remove_anchored_selection(buffer, stored);
    view.selection_history_.pop_back();
    return result;
}

void ViewRegistry::clear_selection_history(ViewId id) {
    clear_selection_history(get(id));
}

void ViewRegistry::clear_selection_history(View& view) {
    Buffer& buffer = buffers_->get(view.buffer_id_);
    for (View::AnchoredSelection& stored : view.selection_history_) {
        remove_anchored_selection(buffer, stored);
    }
    view.selection_history_.clear();
}

std::size_t ViewRegistry::selection_history_size(ViewId id) const {
    return get(id).selection_history_.size();
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
