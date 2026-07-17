#pragma once

#include "async/runtime.hpp"
#include "editor/command.hpp"

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
    CommandTarget origin;
    WindowId window;
    BufferId buffer;
    ViewId view;
    std::vector<InteractionCandidate> candidates;
    std::size_t selected = 0;
    std::uint64_t generation = 0;
    bool loading = false;
    std::string error;
};

struct InteractionSubmission {
    CommandId accept_command;
    CommandInvocation invocation;
    CommandTarget target;
};

// Owns a transient minibuffer Buffer/View/Window while an interaction is
// active. The input is ordinary editor state; providers and frontends observe
// it through the same registries as every other editable surface.
class InteractionController {
public:
    InteractionController(EditorRuntime& runtime, InteractionProviderRegistry& providers)
        : runtime_(&runtime), providers_(&providers) {}
    ~InteractionController() noexcept { (void)cancel(); }

    InteractionController(const InteractionController&) = delete;
    InteractionController& operator=(const InteractionController&) = delete;

    void attach_async_runtime(AsyncRuntime& runtime) { async_runtime_ = &runtime; }

    bool active() const { return state_.has_value(); }
    InteractionState* state() { return state_ ? &*state_ : nullptr; }
    const InteractionState* state() const { return state_ ? &*state_ : nullptr; }

    std::expected<void, std::string> start(InteractionRequest request, CommandContext& context,
                                           KeymapId keymap);
    std::string input_text() const;
    TextOffset input_caret() const;
    RevisionId input_revision() const;
    bool move_selection(int delta);
    bool select(std::size_t index);
    void refresh_candidates() { refresh(); }
    std::expected<InteractionSubmission, std::string> submit();
    bool cancel() noexcept;

    const std::vector<std::string>& history(std::string_view name) const;

private:
    void refresh();
    void destroy_surface(WindowId window, ViewId view, BufferId buffer) noexcept;
    void cancel_pending() noexcept;
    void apply_candidates(std::uint64_t generation, std::vector<InteractionCandidate> candidates);
    void apply_failure(std::uint64_t generation, const std::exception_ptr& failure);
    static std::vector<InteractionCandidate> rank(std::vector<InteractionCandidate> candidates,
                                                  std::string_view query,
                                                  const std::stop_token* cancellation = nullptr);

    EditorRuntime* runtime_;
    InteractionProviderRegistry* providers_;
    AsyncRuntime* async_runtime_ = nullptr;
    AsyncTaskId pending_task_;
    std::uint64_t next_generation_ = 0;
    std::optional<InteractionState> state_;
    std::unordered_map<std::string, std::vector<std::string>> histories_;
};

} // namespace cind
