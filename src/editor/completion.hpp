#pragma once

#include "async/runtime.hpp"
#include "document/text_types.hpp"
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

enum class CompletionProviderKind : std::uint8_t {
    Lsp,
    Path,
    Word,
    Snippet,
    Scripted,
};

struct CompletionProvider {
    CompletionProviderKind kind = CompletionProviderKind::Word;
    std::uint64_t session = 0;

    static constexpr CompletionProvider word() { return {CompletionProviderKind::Word, 0}; }
    static constexpr CompletionProvider path() { return {CompletionProviderKind::Path, 0}; }
    static constexpr CompletionProvider snippet() { return {CompletionProviderKind::Snippet, 0}; }
    static constexpr CompletionProvider lsp(std::uint64_t session) {
        return {CompletionProviderKind::Lsp, session};
    }
    static constexpr CompletionProvider scripted(std::uint64_t provider) {
        return {CompletionProviderKind::Scripted, provider};
    }

    friend constexpr auto operator<=>(CompletionProvider, CompletionProvider) = default;
};

std::string completion_provider_name(CompletionProvider provider);

enum class CompletionTriggerKind : std::uint8_t {
    Manual,
    Character,
    Automatic,
    Incomplete,
};

struct CompletionTrigger {
    CompletionTriggerKind kind = CompletionTriggerKind::Manual;
    std::string character;
};

struct CompletionRequest {
    CommandTarget target;
    RevisionId revision = 0;
    TextOffset anchor;
    TextOffset caret;
    std::uint32_t line = 0;
    std::string query;
    CompletionTrigger trigger;
    std::uint64_t generation = 0;
};

struct CompletionEdit {
    TextRange insert_range;
    TextRange replace_range;
    std::string new_text;
};

struct CompletionItem {
    std::uint64_t id = 0;
    CompletionProvider provider;
    std::string filter_text;
    std::string label;
    std::string kind;
    std::string detail;
    std::optional<CompletionEdit> edit;
    std::string sort_text;
    bool is_snippet = false;
    bool resolved = true;
    bool resolving = false;
    std::string resolve_error;
    std::string documentation;
    std::vector<TextEdit> additional_edits;
    // Type-erased provider payload. The owning provider interprets it
    // when resolve and provider-specific application are implemented.
    std::string raw;
};

struct CompletionProviderResponse {
    CompletionProvider provider;
    std::vector<CompletionItem> items;
    bool is_incomplete = false;
};

using CompletionProviderWork = std::function<CompletionProviderResponse(const std::stop_token&)>;

struct CompletionProviderAsync {
    using Completed = std::function<void(CompletionProviderResponse)>;
    using Failed = std::function<void(std::string)>;
    using Cancelled = std::function<void()>;
    using Cancel = std::function<void()>;
    using Start = std::function<std::expected<Cancel, std::string>(Completed, Failed, Cancelled)>;

    Start start;
};

using CompletionProviderResult =
    std::variant<CompletionProviderResponse, CompletionProviderWork, CompletionProviderAsync>;

struct CompletionMatch {
    CompletionItem item;
    int tier = 0;
    int score = 0;
};

struct CompletionProviderState {
    CompletionProvider provider;
    std::vector<CompletionItem> items;
    bool pending = false;
    bool is_incomplete = false;
    std::string error;
};

struct CompletionState {
    CompletionRequest request;
    std::vector<CompletionProviderState> providers;
    std::vector<CompletionMatch> matches;
};

struct CompletionApplyOptions {
    bool replace = false;
};

// Frontend-independent completion pipeline. It owns request generations,
// provider replacement, unified filtering/sorting, and atomic edit
// application. Provider registration and dispatch remain outside the pipeline.
class CompletionPipeline {
public:
    using Dispatch =
        std::function<CompletionProviderResult(CompletionProvider, const CompletionRequest&)>;
    using ResolveCompleted = std::function<void(CompletionItem)>;
    using Resolve = std::function<std::expected<CompletionProviderAsync::Cancel, std::string>(
        const CompletionRequest&, const CompletionItem&, ResolveCompleted,
        CompletionProviderAsync::Failed, CompletionProviderAsync::Cancelled)>;
    using Applied = std::function<void(CommandTarget)>;
    using Changed = std::function<void(const CompletionState*)>;

    CompletionPipeline(EditorRuntime& runtime, AsyncRuntime& async_runtime, Dispatch dispatch,
                       Resolve resolve = {}, Applied applied = {}, Changed changed = {});
    ~CompletionPipeline() noexcept { (void)cancel(); }

    CompletionPipeline(const CompletionPipeline&) = delete;
    CompletionPipeline& operator=(const CompletionPipeline&) = delete;

    bool active() const { return state_.has_value(); }
    CompletionState* state() { return state_ ? &*state_ : nullptr; }
    const CompletionState* state() const { return state_ ? &*state_ : nullptr; }

    std::expected<void, std::string> start(CommandContext& context, TextOffset anchor,
                                           std::vector<CompletionProvider> providers,
                                           CompletionTrigger trigger = {});
    // Advances a complete session without provider RPC when input remains a
    // prefix extension. Incomplete sources are re-requested and replace only
    // their own entries.
    std::expected<void, std::string> update(CommandContext& context);
    bool focus(std::size_t index);
    void resolve_visible(std::size_t first, std::size_t count,
                         std::optional<std::size_t> selected = std::nullopt);
    std::expected<void, std::string> apply(std::size_t selected,
                                           CompletionApplyOptions options = {});
    bool cancel() noexcept;
    bool invalidate_if_stale();

private:
    void request_provider(CompletionProvider provider, const CompletionRequest& request);
    void publish(std::uint64_t generation, CompletionProviderResponse response);
    void fail(std::uint64_t generation, CompletionProvider provider, std::string error);
    void cancelled(std::uint64_t generation, CompletionProvider provider);
    void rebuild_matches();
    void notify_changed() noexcept;
    void settle_automatic();
    void resolve_item(std::uint64_t id);
    void publish_resolved(std::uint64_t generation, std::uint64_t id, CompletionItem item);
    void resolve_failed(std::uint64_t generation, std::uint64_t id, std::string error);
    void resolve_cancelled(std::uint64_t generation, std::uint64_t id);
    void cancel_pending() noexcept;
    CompletionItem* source_item(std::uint64_t id);
    CompletionProviderState* source(CompletionProvider provider);
    const CompletionProviderState* source(CompletionProvider provider) const;
    std::expected<void, std::string> validate_response(CompletionProviderResponse& response);

    EditorRuntime* runtime_;
    AsyncRuntime* async_runtime_;
    Dispatch dispatch_;
    Resolve resolve_;
    Applied applied_;
    Changed changed_;
    std::uint64_t next_generation_ = 0;
    std::uint64_t next_item_id_ = 0;
    bool dispatching_batch_ = false;
    std::optional<CompletionState> state_;
    std::vector<std::function<void()>> pending_cancellations_;
    std::unordered_map<std::uint64_t, std::function<void()>> resolve_cancellations_;
    struct PendingApply {
        std::uint64_t generation = 0;
        std::uint64_t item = 0;
        CompletionApplyOptions options;
    };
    std::optional<PendingApply> pending_apply_;
};

} // namespace cind
