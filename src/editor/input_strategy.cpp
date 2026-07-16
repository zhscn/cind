#include "editor/input_strategy.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

void InputStrategyRegistry::validate(const Definition& definition) const {
    if (definition.name.empty()) {
        throw std::invalid_argument("input strategy name must not be empty");
    }
    (void)input_states_->definition(definition.editing);
    (void)input_states_->definition(definition.interface);
}

InputStrategyId InputStrategyRegistry::define(Definition definition) {
    if (sealed_) {
        throw std::logic_error("input strategy registry is sealed");
    }
    validate(definition);
    if (by_name_.contains(definition.name)) {
        throw std::invalid_argument(
            std::format("input strategy '{}' is already defined", definition.name));
    }
    const InputStrategyId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back(std::move(definition));
    by_name_.emplace(definitions_.back().name, id);
    return id;
}

void InputStrategyRegistry::configure(InputStrategyId id, Definition definition) {
    if (sealed_) {
        throw std::logic_error("input strategy registry is sealed");
    }
    validate(definition);
    Definition& current = const_cast<Definition&>(this->definition(id));
    if (definition.name != current.name) {
        throw std::invalid_argument("input strategy configuration cannot change its name");
    }
    current = std::move(definition);
}

const InputStrategyRegistry::Definition&
InputStrategyRegistry::definition(InputStrategyId id) const {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown input strategy id");
    }
    return definitions_[id.value];
}

std::optional<InputStrategyId> InputStrategyRegistry::find(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return found == by_name_.end() ? std::nullopt : std::optional(found->second);
}

InputStateId InputStrategyRegistry::state(InputStrategyId strategy,
                                          InteractionClass interaction_class) const {
    const Definition& selected = definition(strategy);
    return interaction_class == InteractionClass::Editing ? selected.editing : selected.interface;
}

void InputStrategyRegistry::set_default(std::optional<InputStrategyId> strategy) {
    if (strategy) {
        (void)definition(*strategy);
    }
    default_ = strategy;
}

} // namespace cind
