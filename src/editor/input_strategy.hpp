#pragma once

#include "editor/input_state.hpp"
#include "editor/mode.hpp"

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

struct InputStrategyId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(InputStrategyId, InputStrategyId) = default;
};

enum class SelectionEditPolicy : std::uint8_t {
    Collapse,
    Preserve,
};

constexpr std::string_view selection_edit_policy_name(SelectionEditPolicy policy) {
    switch (policy) {
    case SelectionEditPolicy::Collapse:
        return "collapse";
    case SelectionEditPolicy::Preserve:
        return "preserve";
    }
    return "unknown";
}

// A strategy maps content interaction classes to durable input states. Modes
// describe a Buffer; a View selects the strategy used to interpret that
// description, so multiple editing schemes can coexist in one application.
class InputStrategyRegistry {
public:
    struct Definition {
        std::string name;
        InputStateId editing;
        InputStateId interface;
        SelectionEditPolicy selection_after_edit = SelectionEditPolicy::Collapse;
    };

    explicit InputStrategyRegistry(const InputStateRegistry& input_states)
        : input_states_(&input_states) {}

    InputStrategyId define(Definition definition);
    void configure(InputStrategyId id, Definition definition);
    const Definition& definition(InputStrategyId id) const;
    std::optional<InputStrategyId> find(std::string_view name) const;
    InputStateId state(InputStrategyId strategy, InteractionClass interaction_class) const;

    void set_default(std::optional<InputStrategyId> strategy);
    std::optional<InputStrategyId> default_strategy() const { return default_; }

    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

private:
    void validate(const Definition& definition) const;

    const InputStateRegistry* input_states_;
    std::vector<Definition> definitions_;
    std::unordered_map<std::string, InputStrategyId> by_name_;
    std::optional<InputStrategyId> default_;
    bool sealed_ = false;
};

} // namespace cind
