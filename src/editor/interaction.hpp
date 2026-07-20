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

struct InteractionCandidateAsync {
    using Completed = std::function<void(std::vector<InteractionCandidate>)>;
    using Failed = std::function<void(std::string)>;
    using Cancelled = std::function<void()>;
    using Cancel = std::function<void()>;
    using Start = std::function<std::expected<Cancel, std::string>(Completed, Failed, Cancelled)>;

    Start start;
};

using InteractionProviderResult = std::variant<std::vector<InteractionCandidate>,
                                               InteractionCandidateWork, InteractionCandidateAsync>;

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

// Native state for the transient editor surface and provider execution. Guile
// owns the interaction policy record, including prompts, history, submission,
// and candidate selection.
struct InteractionMechanismState {
    bool uses_candidates = false;
    CommandTarget origin;
    WindowId window;
    BufferId buffer;
    ViewId view;
    std::vector<InteractionCandidate> candidates;
    std::uint64_t candidate_revision = 0;
    std::uint64_t generation = 0;
    bool loading = false;
    std::string error;
};

// Owns a transient minibuffer Buffer/View/Window while an interaction is
// active. The input is ordinary editor state; providers and frontends observe
// it through the same registries as every other editable surface.
class InteractionMechanisms {
public:
    InteractionMechanisms(EditorRuntime& runtime, InteractionProviderRegistry& providers)
        : runtime_(&runtime), providers_(&providers) {}
    ~InteractionMechanisms() noexcept { (void)cancel(); }

    InteractionMechanisms(const InteractionMechanisms&) = delete;
    InteractionMechanisms& operator=(const InteractionMechanisms&) = delete;

    void attach_async_runtime(AsyncRuntime& runtime) { async_runtime_ = &runtime; }

    bool active() const { return state_.has_value(); }
    InteractionMechanismState* state() { return state_ ? &*state_ : nullptr; }
    const InteractionMechanismState* state() const { return state_ ? &*state_ : nullptr; }

    std::expected<void, std::string> start(InteractionRequest request, CommandContext& context);
    std::string input_text() const;
    TextOffset input_caret() const;
    RevisionId input_revision() const;
    std::expected<RevisionId, std::string> replace_input(std::string_view input);
    std::expected<void, std::string> refresh_candidates(std::string_view provider);
    std::expected<std::string, std::string> submit(std::optional<std::size_t> selected,
                                                   bool allow_custom_input);
    bool cancel() noexcept;

private:
    void refresh(std::string_view provider);
    void destroy_surface(WindowId window, ViewId view, BufferId buffer) noexcept;
    void cancel_pending() noexcept;
    void apply_candidates(std::uint64_t generation, std::vector<InteractionCandidate> candidates);
    void apply_failure(std::uint64_t generation, const std::exception_ptr& failure);

    EditorRuntime* runtime_;
    InteractionProviderRegistry* providers_;
    AsyncRuntime* async_runtime_ = nullptr;
    std::function<void()> cancel_pending_task_;
    std::uint64_t next_generation_ = 0;
    std::optional<InteractionMechanismState> state_;
};

} // namespace cind
