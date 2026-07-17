#pragma once

#include "editor/ids.hpp"
#include "editor/keymap.hpp"
#include "presentation/cursor_shape.hpp"
#include "presentation/position_hint.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

struct InputStateId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(InputStateId, InputStateId) = default;
};

enum class TextInputPolicy : std::uint8_t {
    Accept,
    Ignore,
};

enum class InputStateChangeKind : std::uint8_t {
    Push,
    Pop,
    Base,
};

struct InputStateChange {
    ViewId view;
    InputStateChangeKind kind = InputStateChangeKind::Base;
    std::optional<InputStateId> from;
    std::optional<InputStateId> to;

    friend constexpr bool operator==(const InputStateChange&, const InputStateChange&) = default;
};

enum class InputStateHandlerActionKind : std::uint8_t {
    Pass,
    Consume,
    Dispatch,
    Pending,
};

struct InputHint {
    std::string key;
    std::string detail;
    bool prefix = false;

    friend bool operator==(const InputHint&, const InputHint&) = default;
};

struct InputFeedback {
    std::string sequence;
    std::vector<InputHint> hints;
};

struct InputStateHandlerAction {
    InputStateHandlerActionKind kind = InputStateHandlerActionKind::Pass;
    CommandId command;
    CommandInvocation invocation;
    std::optional<InputFeedback> feedback;
};

using InputStateHandlerResult = std::expected<InputStateHandlerAction, std::string>;
using InputStateHandler = std::function<InputStateHandlerResult(CommandContext&, KeyStroke)>;
// Lifecycle callbacks belong to stack membership rather than top-state
// activation: enter runs when a state is inserted and exit runs when it is
// removed. A transient state may therefore obscure another state without
// ending the obscured state's lifetime.
using InputStateLifecycleHandler = std::function<void(const InputStateChange&)>;
using PositionHintProviderResult = std::expected<std::vector<PositionHint>, std::string>;
// Position hints are presentation policy derived from the current document,
// Selection, effective mode policy, and InputState. Providers are pure queries;
// the application memoizes their result across equivalent presentation frames.
using PositionHintProvider = std::function<PositionHintProviderResult(CommandContext&)>;

class InputStateRegistry {
public:
    struct Definition {
        std::string name;
        std::vector<KeymapId> keymaps;
        TextInputPolicy text_input = TextInputPolicy::Accept;
        CursorShape cursor = CursorShape::Beam;
        std::string indicator;
        InputStateHandler handler;
        PositionHintProvider position_hints;
        InputStateLifecycleHandler on_enter;
        InputStateLifecycleHandler on_exit;
    };

    using ListenerId = std::uint64_t;
    using Listener = std::function<void(const InputStateChange&)>;

    explicit InputStateRegistry(const KeymapRegistry& keymaps) : keymaps_(&keymaps) {}

    InputStateId define(Definition definition);
    void configure(InputStateId id, Definition definition);
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(InputStateId id) const;
    Definition& definition_for_configuration(InputStateId id);
    std::optional<InputStateId> find(std::string_view name) const;

    ListenerId subscribe(Listener listener);
    bool unsubscribe(ListenerId listener);

private:
    friend class ViewInputStates;

    void enter(InputStateId state, const InputStateChange& change) const noexcept;
    void exit(InputStateId state, const InputStateChange& change) const noexcept;
    void publish(const InputStateChange& change) const;
    void validate(const Definition& definition) const;

    const KeymapRegistry* keymaps_;
    std::vector<Definition> definitions_;
    std::unordered_map<std::string, InputStateId> by_name_;
    std::vector<std::pair<ListenerId, Listener>> listeners_;
    ListenerId next_listener_ = 1;
    bool sealed_ = false;
};

class ViewInputStates {
public:
    bool empty() const { return states_.empty(); }
    std::optional<InputStateId> base() const;
    std::optional<InputStateId> top() const;
    const std::vector<InputStateId>& stack() const { return states_; }
    const std::optional<InputFeedback>& feedback() const { return feedback_; }

private:
    friend class ViewRegistry;

    void set_base(InputStateRegistry& registry, ViewId view, InputStateId state);
    void push(InputStateRegistry& registry, ViewId view, InputStateId state);
    std::optional<InputStateId> pop(InputStateRegistry& registry, ViewId view);
    void reset(InputStateRegistry& registry, ViewId view);
    void release(InputStateRegistry& registry, ViewId view);
    void set_feedback(InputFeedback feedback);
    void clear_feedback() { feedback_.reset(); }

    std::vector<InputStateId> states_;
    std::optional<InputFeedback> feedback_;
};

} // namespace cind
