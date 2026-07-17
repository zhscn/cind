#pragma once

#include "async/runtime.hpp"
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
#include <variant>
#include <vector>

namespace cind {

struct InteractionCandidate {
    std::string value;
    std::string label;
    std::string detail;
    std::string filter_text;
};

using InteractionCandidateWork =
    std::function<std::vector<InteractionCandidate>(const std::stop_token&)>;
using InteractionProviderResult =
    std::variant<std::vector<InteractionCandidate>, InteractionCandidateWork>;

class InteractionProviderRegistry {
public:
    // Provider preparation runs on the editor thread. It may return immediate
    // candidates or a worker callback that captures only immutable input.
    using Complete = std::function<InteractionProviderResult(CommandContext&, std::string_view)>;

    void define(std::string name, Complete complete);
    void configure(std::string_view name, Complete complete);
    bool contains(std::string_view name) const;
    InteractionProviderResult complete(std::string_view name, CommandContext& context,
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
    bool loading = false;
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

    void attach_async_runtime(AsyncRuntime& runtime) { async_runtime_ = &runtime; }

    bool active() const { return state_.has_value(); }
    InteractionState* state() { return state_ ? &*state_ : nullptr; }
    const InteractionState* state() const { return state_ ? &*state_ : nullptr; }

    std::expected<void, std::string> start(InteractionRequest request, CommandContext& context);
    void insert(std::string_view text, CommandContext& context);
    bool erase_backward(CommandContext& context);
    bool erase_forward(CommandContext& context);
    bool move_backward();
    bool move_forward();
    bool move_word_backward();
    bool move_word_forward();
    bool move_to_start();
    bool move_to_end();
    bool kill_to_end(CommandContext& context);
    bool yank(CommandContext& context);
    bool move_selection(int delta);
    bool select(std::size_t index);
    void refresh_candidates(CommandContext& context) { refresh(context); }
    std::expected<InteractionSubmission, std::string> submit();
    bool cancel();

    const std::vector<std::string>& history(std::string_view name) const;

private:
    void refresh(CommandContext& context);
    void cancel_pending() noexcept;
    void apply_candidates(std::uint64_t generation, std::vector<InteractionCandidate> candidates);
    void apply_failure(std::uint64_t generation, const std::exception_ptr& failure);
    static std::vector<InteractionCandidate> rank(std::vector<InteractionCandidate> candidates,
                                                  std::string_view query,
                                                  const std::stop_token* cancellation = nullptr);

    InteractionProviderRegistry* providers_;
    AsyncRuntime* async_runtime_ = nullptr;
    AsyncTaskId pending_task_;
    std::uint64_t next_generation_ = 0;
    std::optional<InteractionState> state_;
    std::unordered_map<std::string, std::vector<std::string>> histories_;
    std::string last_kill_;
};

} // namespace cind
