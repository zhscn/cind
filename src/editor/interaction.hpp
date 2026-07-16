#pragma once

#include "editor/command.hpp"
#include "editor/text_input.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

struct InteractionCandidate {
    std::string value;
    std::string label;
    std::string detail;
    std::string filter_text;
};

class InteractionProviderRegistry {
public:
    using Complete =
        std::function<std::vector<InteractionCandidate>(CommandContext&, std::string_view)>;

    void define(std::string name, Complete complete);
    bool contains(std::string_view name) const;
    std::vector<InteractionCandidate> complete(std::string_view name, CommandContext& context,
                                               std::string_view query) const;

    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

private:
    std::unordered_map<std::string, Complete> providers_;
    bool sealed_ = false;
};

struct InteractionState {
    InteractionRequest request;
    TextInput input;
    std::vector<InteractionCandidate> candidates;
    std::size_t selected = 0;
    std::uint64_t generation = 0;
    std::string error;
};

struct InteractionSubmission {
    CommandId accept_command;
    CommandInvocation invocation;
};

// Owns the active prompt or picker independently of any frontend, including
// its editable text and caret. Providers produce semantic candidates;
// frontends only render the state and translate navigation/input events.
class InteractionController {
public:
    explicit InteractionController(InteractionProviderRegistry& providers)
        : providers_(&providers) {}

    bool active() const { return state_.has_value(); }
    const InteractionState* state() const { return state_ ? &*state_ : nullptr; }

    std::expected<void, std::string> start(InteractionRequest request, CommandContext& context);
    void insert(std::string_view text, CommandContext& context);
    bool erase_backward(CommandContext& context);
    bool erase_forward(CommandContext& context);
    bool move_backward();
    bool move_forward();
    bool move_to_start();
    bool move_to_end();
    bool move_selection(int delta);
    bool select(std::size_t index);
    std::expected<InteractionSubmission, std::string> submit();
    bool cancel();

    const std::vector<std::string>& history(std::string_view name) const;

private:
    void refresh(CommandContext& context);
    static std::vector<InteractionCandidate> rank(std::vector<InteractionCandidate> candidates,
                                                  std::string_view query);

    InteractionProviderRegistry* providers_;
    std::optional<InteractionState> state_;
    std::unordered_map<std::string, std::vector<std::string>> histories_;
};

} // namespace cind
