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
#include <variant>
#include <vector>

namespace cind {

enum class CompletionProviderKind : std::uint8_t {
    Lsp,
    Path,
    Word,
    Snippet,
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
    std::string documentation;
    std::vector<TextEdit> additional_edits;
    // Type-erased provider payload. The owning fixed provider interprets it
    // when resolve and provider-specific application are implemented.
    std::string raw;
};

struct CompletionProviderResponse {
    CompletionProvider provider;
    std::vector<CompletionItem> items;
    bool is_incomplete = false;
};

using CompletionProviderWork =
    std::function<CompletionProviderResponse(const std::stop_token&)>;

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
    std::size_t selected = 0;
};

struct CompletionApplyOptions {
    bool replace = false;
};

// Fixed-provider completion pipeline. It owns request generations, source
// replacement, unified filtering/sorting, and atomic edit application. The
// dispatcher is a compiled mechanism switch, not a dynamic plugin registry.
class CompletionPipeline {
public:
    using Dispatch =
        std::function<CompletionProviderResult(CompletionProvider, const CompletionRequest&)>;

    CompletionPipeline(EditorRuntime& runtime, AsyncRuntime& async_runtime, Dispatch dispatch);
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
    bool select(std::size_t index);
    bool select_relative(std::int64_t delta);
    std::expected<void, std::string> apply(CompletionApplyOptions options = {});
    bool cancel() noexcept;
    bool invalidate_if_stale();

private:
    void request_provider(CompletionProvider provider, const CompletionRequest& request);
    void publish(std::uint64_t generation, CompletionProviderResponse response);
    void fail(std::uint64_t generation, CompletionProvider provider, std::string error);
    void cancelled(std::uint64_t generation, CompletionProvider provider);
    void rebuild_matches();
    void cancel_pending() noexcept;
    CompletionProviderState* source(CompletionProvider provider);
    const CompletionProviderState* source(CompletionProvider provider) const;
    std::expected<void, std::string> validate_response(CompletionProviderResponse& response);

    EditorRuntime* runtime_;
    AsyncRuntime* async_runtime_;
    Dispatch dispatch_;
    std::uint64_t next_generation_ = 0;
    std::uint64_t next_item_id_ = 0;
    std::optional<CompletionState> state_;
    std::vector<std::function<void()>> pending_cancellations_;
};

} // namespace cind
