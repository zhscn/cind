#include "editor/input_state.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

void InputStateRegistry::validate(const Definition& definition) const {
    if (definition.name.empty()) {
        throw std::invalid_argument("input state name must not be empty");
    }
    for (const KeymapId keymap : definition.keymaps) {
        (void)keymaps_->definition(keymap);
    }
}

InputStateId InputStateRegistry::define(Definition definition) {
    if (sealed_) {
        throw std::logic_error("input state registry is sealed");
    }
    validate(definition);
    if (by_name_.contains(definition.name)) {
        throw std::invalid_argument(
            std::format("input state '{}' is already defined", definition.name));
    }
    const InputStateId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back(std::move(definition));
    by_name_.emplace(definitions_.back().name, id);
    return id;
}

const InputStateRegistry::Definition& InputStateRegistry::definition(InputStateId id) const {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown input state id");
    }
    return definitions_[id.value];
}

InputStateRegistry::Definition& InputStateRegistry::definition_for_configuration(InputStateId id) {
    if (sealed_) {
        throw std::logic_error("input state registry is sealed");
    }
    return const_cast<Definition&>(std::as_const(*this).definition(id));
}

std::optional<InputStateId> InputStateRegistry::find(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return found == by_name_.end() ? std::nullopt : std::optional(found->second);
}

InputStateRegistry::ListenerId InputStateRegistry::subscribe(Listener listener) {
    if (!listener) {
        throw std::invalid_argument("input state listener must be callable");
    }
    if (next_listener_ == 0) {
        throw std::overflow_error("input state listener registry is exhausted");
    }
    const ListenerId id = next_listener_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

bool InputStateRegistry::unsubscribe(ListenerId listener) {
    const auto found = std::ranges::find_if(
        listeners_, [listener](const auto& entry) { return entry.first == listener; });
    if (found == listeners_.end()) {
        return false;
    }
    listeners_.erase(found);
    return true;
}

void InputStateRegistry::publish(const InputStateChange& change) const {
    std::vector<Listener> listeners;
    listeners.reserve(listeners_.size());
    for (const auto& [id, listener] : listeners_) {
        (void)id;
        listeners.push_back(listener);
    }
    for (const Listener& listener : listeners) {
        listener(change);
    }
}

std::optional<InputStateId> ViewInputStates::base() const {
    return states_.empty() ? std::nullopt : std::optional(states_.front());
}

std::optional<InputStateId> ViewInputStates::top() const {
    return states_.empty() ? std::nullopt : std::optional(states_.back());
}

void ViewInputStates::set_base(InputStateRegistry& registry, ViewId view, InputStateId state) {
    (void)registry.definition(state);
    const std::optional<InputStateId> previous = base();
    if (states_.empty()) {
        states_.push_back(state);
    } else {
        states_.front() = state;
    }
    if (previous != state) {
        registry.publish(
            {.view = view, .kind = InputStateChangeKind::Base, .from = previous, .to = state});
    }
}

void ViewInputStates::push(InputStateRegistry& registry, ViewId view, InputStateId state) {
    (void)registry.definition(state);
    if (states_.empty()) {
        throw std::logic_error("input state stack requires a base before push");
    }
    const std::optional<InputStateId> previous = top();
    states_.push_back(state);
    registry.publish(
        {.view = view, .kind = InputStateChangeKind::Push, .from = previous, .to = state});
}

std::optional<InputStateId> ViewInputStates::pop(InputStateRegistry& registry, ViewId view) {
    if (states_.size() <= 1) {
        return std::nullopt;
    }
    const InputStateId removed = states_.back();
    states_.pop_back();
    registry.publish(
        {.view = view, .kind = InputStateChangeKind::Pop, .from = removed, .to = states_.back()});
    return removed;
}

void ViewInputStates::reset(InputStateRegistry& registry, ViewId view) {
    while (pop(registry, view)) {
    }
}

} // namespace cind
