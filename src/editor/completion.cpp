#include "editor/completion.hpp"

#include "editor/runtime.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace cind {

namespace {

std::string ascii_lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(static_cast<char>(std::tolower(byte)));
    }
    return result;
}

std::optional<std::pair<int, int>> fuzzy_score(std::string_view candidate, std::string_view query) {
    const std::string haystack = ascii_lower(candidate);
    const std::string needle = ascii_lower(query);
    if (needle.empty()) {
        return std::pair{0, 0};
    }
    std::size_t position = 0;
    int score = 0;
    int consecutive = 0;
    std::size_t first = std::string::npos;
    for (const char wanted : needle) {
        const std::size_t found = haystack.find(wanted, position);
        if (found == std::string::npos) {
            return std::nullopt;
        }
        if (first == std::string::npos) {
            first = found;
        }
        if (found == position) {
            ++consecutive;
            score += 12 + consecutive * 3;
        } else {
            consecutive = 0;
            score -= static_cast<int>(found - position);
        }
        if (found == 0 || !std::isalnum(static_cast<unsigned char>(haystack[found - 1]))) {
            score += 18;
        }
        position = found + 1;
    }
    score -= static_cast<int>(haystack.size() - needle.size());
    const bool exact = haystack == needle;
    const bool prefix = haystack.starts_with(needle);
    const bool word_start =
        first == 0 || (first != std::string::npos && first > 0 &&
                       !std::isalnum(static_cast<unsigned char>(haystack[first - 1])));
    const int tier = exact ? 0 : prefix ? 1 : word_start ? 2 : 3;
    return std::pair{tier, score};
}

bool overlaps(TextRange left, TextRange right) {
    return left.start < right.end && right.start < left.end;
}

} // namespace

std::string completion_provider_name(CompletionProvider provider) {
    switch (provider.kind) {
    case CompletionProviderKind::Lsp:
        return std::format("lsp:{}", provider.session);
    case CompletionProviderKind::Path:
        return "path";
    case CompletionProviderKind::Word:
        return "word";
    case CompletionProviderKind::Snippet:
        return "snippet";
    }
    throw std::logic_error("unknown completion provider kind");
}

CompletionPipeline::CompletionPipeline(EditorRuntime& runtime, AsyncRuntime& async_runtime,
                                       Dispatch dispatch, Resolve resolve, Applied applied)
    : runtime_(&runtime), async_runtime_(&async_runtime), dispatch_(std::move(dispatch)),
      resolve_(std::move(resolve)), applied_(std::move(applied)) {
    if (!dispatch_) {
        throw std::invalid_argument("completion pipeline requires a provider dispatcher");
    }
}

std::expected<void, std::string>
CompletionPipeline::start(CommandContext& context, TextOffset anchor,
                          std::vector<CompletionProvider> providers, CompletionTrigger trigger) {
    if (providers.empty()) {
        return std::unexpected("completion requires at least one provider");
    }
    std::ranges::sort(providers);
    if (std::ranges::adjacent_find(providers) != providers.end()) {
        return std::unexpected("completion provider list contains duplicates");
    }
    const DocumentSnapshot snapshot = context.buffer().snapshot();
    const TextOffset caret = runtime_->views().caret(context.view_id());
    if (anchor > caret || caret.value > snapshot.content().size_bytes()) {
        return std::unexpected("completion anchor must precede the caret");
    }
    if (snapshot.content().position(anchor).line != snapshot.content().position(caret).line) {
        return std::unexpected("completion query must remain on one line");
    }

    cancel_pending();
    const std::uint64_t generation = ++next_generation_;
    CompletionRequest request{
        .target = {.window = context.window_id(),
                   .buffer = context.buffer_id(),
                   .view = context.view_id()},
        .revision = snapshot.revision(),
        .anchor = anchor,
        .caret = caret,
        .line = snapshot.content().position(caret).line,
        .query = snapshot.content().substring({anchor, caret}),
        .trigger = std::move(trigger),
        .generation = generation,
    };
    std::vector<CompletionProviderState> sources;
    sources.reserve(providers.size());
    for (CompletionProvider provider : providers) {
        sources.push_back({.provider = provider,
                           .items = {},
                           .pending = false,
                           .is_incomplete = false,
                           .error = {}});
    }
    state_.emplace(CompletionState{
        .request = request, .providers = std::move(sources), .matches = {}, .selected = 0});
    for (CompletionProvider provider : providers) {
        request_provider(provider, request);
    }
    return {};
}

std::expected<void, std::string> CompletionPipeline::update(CommandContext& context) {
    CompletionState* active = state();
    if (active == nullptr) {
        return std::unexpected("completion session is inactive");
    }
    const CompletionRequest previous = active->request;
    if (context.window_id() != previous.target.window ||
        context.buffer_id() != previous.target.buffer ||
        context.view_id() != previous.target.view) {
        (void)cancel();
        return std::unexpected("completion target changed");
    }
    const DocumentSnapshot snapshot = context.buffer().snapshot();
    const TextOffset caret = runtime_->views().caret(context.view_id());
    if (caret < previous.anchor || caret.value > snapshot.content().size_bytes() ||
        snapshot.content().position(caret).line != previous.line) {
        (void)cancel();
        return std::unexpected("completion caret left its query range");
    }
    const std::string query = snapshot.content().substring({previous.anchor, caret});
    const bool related_prefix =
        query.starts_with(previous.query) || previous.query.starts_with(query);
    if (!related_prefix) {
        (void)cancel();
        return std::unexpected("completion query no longer shares its session prefix");
    }

    active->request.revision = snapshot.revision();
    active->request.caret = caret;
    active->request.query = query;
    active->request.generation = ++next_generation_;
    for (CompletionProviderState& provider : active->providers) {
        for (CompletionItem& item : provider.items) {
            if (!item.edit) {
                continue;
            }
            if (item.edit->insert_range.start == previous.anchor &&
                item.edit->insert_range.end == previous.caret) {
                item.edit->insert_range.end = caret;
            }
            if (item.edit->replace_range.start == previous.anchor &&
                item.edit->replace_range.end == previous.caret) {
                item.edit->replace_range.end = caret;
            }
        }
    }
    cancel_pending();
    const CompletionRequest request = active->request;
    for (CompletionProviderState& provider : active->providers) {
        if (provider.is_incomplete) {
            request_provider(
                provider.provider,
                CompletionRequest{
                    .target = request.target,
                    .revision = request.revision,
                    .anchor = request.anchor,
                    .caret = request.caret,
                    .line = request.line,
                    .query = request.query,
                    .trigger = {.kind = CompletionTriggerKind::Incomplete, .character = {}},
                    .generation = request.generation});
        }
    }
    rebuild_matches();
    return {};
}

bool CompletionPipeline::select(std::size_t index) {
    if (!state_ || index >= state_->matches.size()) {
        return false;
    }
    state_->selected = index;
    resolve_visible(index, 1);
    return true;
}

bool CompletionPipeline::select_relative(std::int64_t delta) {
    if (!state_ || state_->matches.empty() || delta == 0) {
        return false;
    }
    const std::int64_t count = static_cast<std::int64_t>(state_->matches.size());
    const std::int64_t selected = static_cast<std::int64_t>(state_->selected);
    state_->selected = static_cast<std::size_t>(((selected + delta) % count + count) % count);
    resolve_visible(state_->selected, 1);
    return true;
}

void CompletionPipeline::resolve_visible(std::size_t first, std::size_t count) {
    if (!state_ || state_->matches.empty() || !resolve_) {
        return;
    }
    first = std::min(first, state_->matches.size());
    const std::size_t end = first + std::min(count, state_->matches.size() - first);
    std::vector<std::uint64_t> items;
    items.reserve(end > first ? end - first + 1 : 1);
    for (std::size_t index = first; index < end; ++index) {
        items.push_back(state_->matches[index].item.id);
    }
    const std::uint64_t selected = state_->matches[state_->selected].item.id;
    if (std::ranges::find(items, selected) == items.end()) {
        items.push_back(selected);
    }
    for (const std::uint64_t id : items) {
        resolve_item(id);
    }
}

std::expected<void, std::string> CompletionPipeline::apply(CompletionApplyOptions options) {
    if (!state_ || state_->matches.empty()) {
        return std::unexpected("no completion candidate is selected");
    }
    const CompletionState state = *state_;
    Buffer* buffer = runtime_->buffers().try_get(state.request.target.buffer);
    View* view = runtime_->views().try_get(state.request.target.view);
    Window* window = runtime_->windows().try_get(state.request.target.window);
    if (buffer == nullptr || view == nullptr || window == nullptr ||
        view->buffer_id() != state.request.target.buffer ||
        window->view_id() != state.request.target.view ||
        buffer->snapshot().revision() != state.request.revision) {
        (void)cancel();
        return std::unexpected("completion request is stale");
    }
    const CompletionItem& item = state.matches[state.selected].item;
    if (!item.resolved) {
        if (!resolve_) {
            return std::unexpected("completion candidate must be resolved before application");
        }
        if (!item.resolve_error.empty()) {
            return std::unexpected(item.resolve_error);
        }
        pending_apply_ = PendingApply{
            .generation = state.request.generation, .item = item.id, .options = options};
        resolve_item(item.id);
        const CompletionItem* pending = source_item(item.id);
        if (pending != nullptr && !pending->resolved && !pending->resolving &&
            !pending->resolve_error.empty()) {
            pending_apply_.reset();
            return std::unexpected(pending->resolve_error);
        }
        return {};
    }
    if (item.is_snippet) {
        return std::unexpected("snippet completion requires the snippet editing state");
    }
    if (!item.edit) {
        return std::unexpected("completion candidate has no text edit");
    }
    const TextEdit primary{options.replace ? item.edit->replace_range : item.edit->insert_range,
                           item.edit->new_text};
    const std::uint32_t text_size = buffer->snapshot().content().size_bytes();
    std::vector<TextEdit> edits = item.additional_edits;
    edits.push_back(primary);
    for (const TextEdit& edit : edits) {
        if (edit.old_range.start > edit.old_range.end || edit.old_range.end.value > text_size) {
            return std::unexpected("completion edit is outside the request snapshot");
        }
    }
    std::ranges::sort(edits, {}, [](const TextEdit& edit) { return edit.old_range.start; });
    for (std::size_t index = 1; index < edits.size(); ++index) {
        if (overlaps(edits[index - 1].old_range, edits[index].old_range)) {
            return std::unexpected("completion edit ranges overlap");
        }
    }

    std::int64_t caret = static_cast<std::int64_t>(primary.old_range.start.value) +
                         static_cast<std::int64_t>(primary.new_text.size());
    for (const TextEdit& edit : item.additional_edits) {
        if (edit.old_range.end <= primary.old_range.start) {
            caret += static_cast<std::int64_t>(edit.new_text.size()) -
                     static_cast<std::int64_t>(edit.old_range.length());
        }
    }
    std::ranges::sort(edits, std::greater{},
                      [](const TextEdit& edit) { return edit.old_range.start.value; });
    EditTransaction transaction = buffer->begin_transaction();
    for (const TextEdit& edit : edits) {
        transaction.replace(edit.old_range, edit.new_text);
    }
    const CommitResult committed = transaction.commit();
    runtime_->views().set_caret(state.request.target.view,
                                TextOffset{static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                                    caret, 0, committed.snapshot.content().size_bytes()))});
    runtime_->views().clear_selection(state.request.target.view);
    (void)cancel();
    if (applied_) {
        applied_();
    }
    return {};
}

bool CompletionPipeline::cancel() noexcept {
    if (!state_) {
        return false;
    }
    cancel_pending();
    state_.reset();
    return true;
}

bool CompletionPipeline::invalidate_if_stale() {
    if (!state_) {
        return false;
    }
    const CompletionRequest& request = state_->request;
    const Buffer* buffer = runtime_->buffers().try_get(request.target.buffer);
    const View* view = runtime_->views().try_get(request.target.view);
    const Window* window = runtime_->windows().try_get(request.target.window);
    if (buffer != nullptr && view != nullptr && window != nullptr &&
        view->buffer_id() == request.target.buffer && window->view_id() == request.target.view &&
        buffer->snapshot().revision() == request.revision &&
        runtime_->views().caret(request.target.view) == request.caret) {
        return false;
    }
    (void)cancel();
    return true;
}

void CompletionPipeline::request_provider(CompletionProvider provider,
                                          const CompletionRequest& request) {
    CompletionProviderState* provider_state = source(provider);
    if (provider_state == nullptr) {
        return;
    }
    provider_state->pending = false;
    provider_state->error.clear();
    try {
        CompletionProviderResult result = dispatch_(provider, request);
        if (CompletionProviderResponse* immediate =
                std::get_if<CompletionProviderResponse>(&result)) {
            publish(request.generation, std::move(*immediate));
            return;
        }
        provider_state->pending = true;
        if (CompletionProviderWork* work = std::get_if<CompletionProviderWork>(&result)) {
            CompletionProviderWork worker = std::move(*work);
            const AsyncTaskId task = async_runtime_->submit({
                .work = [this, generation = request.generation, provider,
                         worker = std::move(worker)](
                            const std::stop_token& stop) mutable -> AsyncCompletion {
                    CompletionProviderResponse response = worker(stop);
                    return [this, generation, provider, response = std::move(response)]() mutable {
                        if (response.provider != provider) {
                            response.provider = provider;
                        }
                        publish(generation, std::move(response));
                    };
                },
                .cancelled = [this, generation = request.generation,
                              provider] { cancelled(generation, provider); },
                .failed =
                    [this, generation = request.generation,
                     provider](const std::exception_ptr& failure) {
                        try {
                            std::rethrow_exception(failure);
                        } catch (const std::exception& exception) {
                            fail(generation, provider, exception.what());
                        } catch (...) {
                            fail(generation, provider, "unknown completion provider failure");
                        }
                    },
            });
            pending_cancellations_.push_back(
                [runtime = async_runtime_, task] { (void)runtime->cancel(task); });
            return;
        }
        CompletionProviderAsync& asynchronous = std::get<CompletionProviderAsync>(result);
        if (!asynchronous.start) {
            fail(request.generation, provider, "completion provider has no start callback");
            return;
        }
        const auto returned = std::make_shared<bool>(false);
        std::expected<CompletionProviderAsync::Cancel, std::string> cancellation =
            asynchronous.start(
                [this, generation = request.generation, provider,
                 returned](CompletionProviderResponse response) mutable {
                    response.provider = provider;
                    publish(generation, std::move(response));
                    *returned = true;
                },
                [this, generation = request.generation, provider,
                 returned](std::string error) mutable {
                    fail(generation, provider, std::move(error));
                    *returned = true;
                },
                [this, generation = request.generation, provider, returned] {
                    cancelled(generation, provider);
                    *returned = true;
                });
        if (!cancellation) {
            if (!*returned) {
                fail(request.generation, provider, std::move(cancellation.error()));
            }
        } else if (!*returned && *cancellation) {
            pending_cancellations_.push_back(std::move(*cancellation));
        }
    } catch (const std::exception& exception) {
        fail(request.generation, provider, exception.what());
    } catch (...) {
        fail(request.generation, provider, "unknown completion provider failure");
    }
}

void CompletionPipeline::publish(std::uint64_t generation, CompletionProviderResponse response) {
    if (!state_ || state_->request.generation != generation) {
        return;
    }
    CompletionProviderState* provider = source(response.provider);
    if (provider == nullptr) {
        return;
    }
    if (std::expected<void, std::string> valid = validate_response(response); !valid) {
        fail(generation, response.provider, std::move(valid.error()));
        return;
    }
    provider->items = std::move(response.items);
    provider->pending = false;
    provider->is_incomplete = response.is_incomplete;
    provider->error.clear();
    rebuild_matches();
    const CompletionState* active = state();
    if (active != nullptr && !active->matches.empty()) {
        resolve_visible(active->selected, 1);
    }
}

void CompletionPipeline::fail(std::uint64_t generation, CompletionProvider provider,
                              std::string error) {
    if (!state_ || state_->request.generation != generation) {
        return;
    }
    if (CompletionProviderState* found = source(provider)) {
        found->pending = false;
        found->error = std::move(error);
    }
}

void CompletionPipeline::cancelled(std::uint64_t generation, CompletionProvider provider) {
    if (!state_ || state_->request.generation != generation) {
        return;
    }
    if (CompletionProviderState* found = source(provider)) {
        found->pending = false;
    }
}

void CompletionPipeline::rebuild_matches() {
    if (!state_) {
        return;
    }
    std::optional<std::uint64_t> selected;
    if (state_->selected < state_->matches.size()) {
        selected = state_->matches[state_->selected].item.id;
    }
    std::vector<CompletionMatch> matches;
    for (const CompletionProviderState& provider : state_->providers) {
        for (const CompletionItem& item : provider.items) {
            const std::string_view filter =
                item.filter_text.empty() ? item.label : item.filter_text;
            const std::optional<std::pair<int, int>> score =
                fuzzy_score(filter, state_->request.query);
            if (score) {
                matches.push_back({.item = item, .tier = score->first, .score = score->second});
            }
        }
    }
    std::ranges::stable_sort(
        matches, [](const CompletionMatch& left, const CompletionMatch& right) {
            return std::tuple(left.tier, -left.score, left.item.sort_text, left.item.kind,
                              left.item.label, left.item.id) <
                   std::tuple(right.tier, -right.score, right.item.sort_text, right.item.kind,
                              right.item.label, right.item.id);
        });
    state_->matches = std::move(matches);
    state_->selected = 0;
    if (selected) {
        const auto found = std::ranges::find(
            state_->matches, *selected, [](const CompletionMatch& match) { return match.item.id; });
        if (found != state_->matches.end()) {
            state_->selected = static_cast<std::size_t>(found - state_->matches.begin());
        }
    }
}

void CompletionPipeline::resolve_item(std::uint64_t id) {
    CompletionItem* item = source_item(id);
    if (item == nullptr || item->resolved || item->resolving || !resolve_ || !state_) {
        return;
    }
    item->resolving = true;
    item->resolve_error.clear();
    rebuild_matches();
    item = source_item(id);
    CompletionState* active = state();
    if (item == nullptr || active == nullptr) {
        return;
    }
    const std::uint64_t generation = active->request.generation;
    const CompletionRequest request = active->request;
    const CompletionItem unresolved = *item;
    const auto returned = std::make_shared<bool>(false);
    try {
        std::expected<CompletionProviderAsync::Cancel, std::string> cancellation = resolve_(
            request, unresolved,
            [this, generation, id, returned](CompletionItem resolved) mutable {
                publish_resolved(generation, id, std::move(resolved));
                *returned = true;
            },
            [this, generation, id, returned](std::string error) mutable {
                resolve_failed(generation, id, std::move(error));
                *returned = true;
            },
            [this, generation, id, returned] {
                resolve_cancelled(generation, id);
                *returned = true;
            });
        if (!cancellation) {
            if (!*returned) {
                resolve_failed(generation, id, std::move(cancellation.error()));
            }
        } else if (!*returned && *cancellation) {
            resolve_cancellations_.insert_or_assign(id, std::move(*cancellation));
        }
    } catch (const std::exception& exception) {
        resolve_failed(generation, id, exception.what());
    } catch (...) {
        resolve_failed(generation, id, "unknown completion resolve failure");
    }
}

void CompletionPipeline::publish_resolved(std::uint64_t generation, std::uint64_t id,
                                          CompletionItem item) {
    if (!state_ || state_->request.generation != generation) {
        return;
    }
    CompletionItem* current = source_item(id);
    if (current == nullptr) {
        return;
    }
    const CompletionProvider provider = current->provider;
    item.id = id;
    item.provider = provider;
    item.resolved = true;
    item.resolving = false;
    item.resolve_error.clear();
    CompletionProviderResponse response{.provider = provider, .items = {std::move(item)}};
    if (std::expected<void, std::string> valid = validate_response(response); !valid) {
        resolve_failed(generation, id, std::move(valid.error()));
        return;
    }
    current = source_item(id);
    if (current == nullptr) {
        return;
    }
    *current = std::move(response.items.front());
    resolve_cancellations_.erase(id);
    rebuild_matches();
    if (pending_apply_ && pending_apply_->generation == generation && pending_apply_->item == id) {
        const CompletionApplyOptions options = pending_apply_->options;
        pending_apply_.reset();
        if (std::expected<void, std::string> applied = apply(options); !applied) {
            resolve_failed(generation, id, std::move(applied.error()));
        }
    }
}

void CompletionPipeline::resolve_failed(std::uint64_t generation, std::uint64_t id,
                                        std::string error) {
    if (!state_ || state_->request.generation != generation) {
        return;
    }
    if (CompletionItem* item = source_item(id)) {
        item->resolving = false;
        item->resolve_error = std::move(error);
        resolve_cancellations_.erase(id);
        rebuild_matches();
    }
    if (pending_apply_ && pending_apply_->generation == generation && pending_apply_->item == id) {
        pending_apply_.reset();
    }
}

void CompletionPipeline::resolve_cancelled(std::uint64_t generation, std::uint64_t id) {
    if (!state_ || state_->request.generation != generation) {
        return;
    }
    if (CompletionItem* item = source_item(id)) {
        item->resolving = false;
        resolve_cancellations_.erase(id);
        rebuild_matches();
    }
    if (pending_apply_ && pending_apply_->generation == generation && pending_apply_->item == id) {
        pending_apply_.reset();
    }
}

void CompletionPipeline::cancel_pending() noexcept {
    pending_apply_.reset();
    std::vector<std::function<void()>> pending = std::move(pending_cancellations_);
    pending_cancellations_.clear();
    for (std::function<void()>& cancellation : pending) {
        try {
            if (cancellation) {
                cancellation();
            }
        } catch (...) {
            continue;
        }
    }
    auto resolves = std::move(resolve_cancellations_);
    resolve_cancellations_.clear();
    for (auto& [id, cancellation] : resolves) {
        (void)id;
        try {
            if (cancellation) {
                cancellation();
            }
        } catch (...) {
            continue;
        }
    }
    if (state_) {
        for (CompletionProviderState& provider : state_->providers) {
            for (CompletionItem& item : provider.items) {
                item.resolving = false;
            }
        }
    }
}

CompletionItem* CompletionPipeline::source_item(std::uint64_t id) {
    if (!state_) {
        return nullptr;
    }
    for (CompletionProviderState& provider : state_->providers) {
        const auto found = std::ranges::find(provider.items, id, &CompletionItem::id);
        if (found != provider.items.end()) {
            return &*found;
        }
    }
    return nullptr;
}

CompletionProviderState* CompletionPipeline::source(CompletionProvider provider) {
    if (!state_) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(state_->providers, provider, &CompletionProviderState::provider);
    return found == state_->providers.end() ? nullptr : &*found;
}

const CompletionProviderState* CompletionPipeline::source(CompletionProvider provider) const {
    if (!state_) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(state_->providers, provider, &CompletionProviderState::provider);
    return found == state_->providers.end() ? nullptr : &*found;
}

std::expected<void, std::string>
CompletionPipeline::validate_response(CompletionProviderResponse& response) {
    CompletionState* active = state();
    if (active == nullptr || source(response.provider) == nullptr) {
        return std::unexpected("completion response came from an unrequested provider");
    }
    const Buffer* buffer = runtime_->buffers().try_get(active->request.target.buffer);
    if (buffer == nullptr || buffer->snapshot().revision() != active->request.revision) {
        return std::unexpected("completion response targets a stale revision");
    }
    const std::uint32_t text_size = buffer->snapshot().content().size_bytes();
    std::set<std::uint64_t> response_ids;
    for (CompletionItem& item : response.items) {
        item.provider = response.provider;
        if (item.id == 0) {
            item.id = ++next_item_id_;
        }
        if (!response_ids.insert(item.id).second) {
            return std::unexpected("completion response contains duplicate item ids");
        }
        if (item.label.empty()) {
            return std::unexpected("completion item label must not be empty");
        }
        if (item.filter_text.empty()) {
            item.filter_text = item.label;
        }
        if (item.sort_text.empty()) {
            item.sort_text = item.label;
        }
        if (item.edit && (item.edit->insert_range.start > item.edit->insert_range.end ||
                          item.edit->replace_range.start > item.edit->replace_range.end ||
                          item.edit->insert_range.end.value > text_size ||
                          item.edit->replace_range.end.value > text_size)) {
            return std::unexpected("completion item edit is outside the request snapshot");
        }
    }
    return {};
}

} // namespace cind
