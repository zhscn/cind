#include "editor/interaction.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

std::vector<std::string> query_terms(std::string_view query) {
    std::vector<std::string> terms;
    std::size_t position = 0;
    while (position < query.size()) {
        while (position < query.size() &&
               std::isspace(static_cast<unsigned char>(query[position]))) {
            ++position;
        }
        const std::size_t start = position;
        while (position < query.size() &&
               !std::isspace(static_cast<unsigned char>(query[position]))) {
            ++position;
        }
        if (start != position) {
            terms.push_back(lowercase(query.substr(start, position - start)));
        }
    }
    return terms;
}

} // namespace

void InteractionProviderRegistry::define(std::string name, Complete complete) {
    if (sealed_) {
        throw std::logic_error("interaction provider registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("interaction provider name must not be empty");
    }
    if (!complete) {
        throw std::invalid_argument("interaction provider implementation must not be empty");
    }
    if (!providers_.emplace(std::move(name), std::move(complete)).second) {
        throw std::invalid_argument("interaction provider is already defined");
    }
}

bool InteractionProviderRegistry::contains(std::string_view name) const {
    return providers_.contains(std::string(name));
}

InteractionProviderResult InteractionProviderRegistry::complete(std::string_view name,
                                                                CommandContext& context,
                                                                std::string_view query) const {
    const auto found = providers_.find(std::string(name));
    if (found == providers_.end()) {
        throw std::out_of_range(std::format("unknown interaction provider '{}'", name));
    }
    return found->second(context, query);
}

std::expected<void, std::string> InteractionController::start(InteractionRequest request,
                                                              CommandContext& context) {
    if (!request.accept_command) {
        return std::unexpected("interaction request has no accept command");
    }
    if (request.kind == InteractionKind::Picker &&
        (request.provider.empty() || !providers_->contains(request.provider))) {
        return std::unexpected(std::format("unknown interaction provider '{}'", request.provider));
    }
    cancel_pending();
    std::string input = request.initial_input;
    state_.emplace(InteractionState{.request = std::move(request),
                                    .input = TextInput(std::move(input)),
                                    .candidates = {},
                                    .selected = 0,
                                    .generation = 0,
                                    .loading = false,
                                    .error = {}});
    refresh(context);
    return {};
}

void InteractionController::insert(std::string_view text, CommandContext& context) {
    if (!state_ || text.empty()) {
        return;
    }
    if (state_->input.insert(text)) {
        refresh(context);
    }
}

bool InteractionController::erase_backward(CommandContext& context) {
    if (!state_ || !state_->input.erase_backward()) {
        return false;
    }
    refresh(context);
    return true;
}

bool InteractionController::erase_forward(CommandContext& context) {
    if (!state_ || !state_->input.erase_forward()) {
        return false;
    }
    refresh(context);
    return true;
}

bool InteractionController::move_backward() {
    return state_ && state_->input.move_backward();
}

bool InteractionController::move_forward() {
    return state_ && state_->input.move_forward();
}

bool InteractionController::move_to_start() {
    return state_ && state_->input.move_to_start();
}

bool InteractionController::move_to_end() {
    return state_ && state_->input.move_to_end();
}

bool InteractionController::move_selection(int delta) {
    if (!state_ || state_->candidates.empty() || delta == 0) {
        return false;
    }
    const std::size_t count = state_->candidates.size();
    const std::int64_t current = static_cast<std::int64_t>(state_->selected);
    const std::int64_t size = static_cast<std::int64_t>(count);
    std::int64_t next = (current + static_cast<std::int64_t>(delta)) % size;
    if (next < 0) {
        next += size;
    }
    state_->selected = static_cast<std::size_t>(next);
    return true;
}

bool InteractionController::select(std::size_t index) {
    if (!state_ || index >= state_->candidates.size()) {
        return false;
    }
    state_->selected = index;
    return true;
}

std::expected<InteractionSubmission, std::string> InteractionController::submit() {
    if (!state_) {
        return std::unexpected("no interaction is active");
    }
    std::string value = state_->input.text();
    if (state_->request.kind == InteractionKind::Picker && !state_->candidates.empty()) {
        value = state_->candidates[state_->selected].value;
    } else if (state_->request.kind == InteractionKind::Picker &&
               !state_->request.allow_custom_input) {
        state_->error = "no matching candidate";
        return std::unexpected(state_->error);
    }

    InteractionSubmission submission{
        .accept_command = state_->request.accept_command,
        .invocation = {.arguments = std::move(state_->request.arguments),
                       .repeat_count = std::nullopt},
    };
    submission.invocation.arguments.emplace_back(value);
    if (!state_->request.history.empty() && !value.empty()) {
        std::vector<std::string>& entries = histories_[state_->request.history];
        if (entries.empty() || entries.back() != value) {
            entries.push_back(value);
            constexpr std::size_t maximum_history = 100;
            if (entries.size() > maximum_history) {
                entries.erase(entries.begin());
            }
        }
    }
    cancel_pending();
    state_.reset();
    return submission;
}

bool InteractionController::cancel() {
    if (!state_) {
        return false;
    }
    cancel_pending();
    state_.reset();
    return true;
}

const std::vector<std::string>& InteractionController::history(std::string_view name) const {
    static const std::vector<std::string> empty;
    const auto found = histories_.find(std::string(name));
    return found == histories_.end() ? empty : found->second;
}

void InteractionController::refresh(CommandContext& context) {
    InteractionState* active = state();
    if (active == nullptr) {
        return;
    }
    cancel_pending();
    InteractionState& state = *active;
    const std::uint64_t generation = ++next_generation_;
    state.generation = generation;
    state.selected = 0;
    state.loading = false;
    state.error.clear();
    state.candidates.clear();
    if (state.request.kind != InteractionKind::Picker) {
        return;
    }
    try {
        InteractionProviderResult result =
            providers_->complete(state.request.provider, context, state.input.text());
        if (auto* candidates = std::get_if<std::vector<InteractionCandidate>>(&result)) {
            state.candidates = rank(std::move(*candidates), state.input.text());
            return;
        }
        if (async_runtime_ == nullptr) {
            state.error = "async interaction provider has no runtime";
            return;
        }
        state.loading = true;
        struct Job {
            std::uint64_t generation = 0;
            std::string query;
            InteractionCandidateWork work;
            InteractionController* controller = nullptr;
        };
        auto job =
            std::make_shared<Job>(Job{.generation = generation,
                                      .query = state.input.text(),
                                      .work = std::move(std::get<InteractionCandidateWork>(result)),
                                      .controller = this});
        pending_task_ = async_runtime_->submit({
            .work = [job](const std::stop_token& cancellation) -> AsyncCompletion {
                std::vector<InteractionCandidate> candidates = job->work(cancellation);
                if (cancellation.stop_requested()) {
                    throw AsyncTaskCancelled();
                }
                candidates =
                    InteractionController::rank(std::move(candidates), job->query, &cancellation);
                if (cancellation.stop_requested()) {
                    throw AsyncTaskCancelled();
                }
                auto shared_candidates =
                    std::make_shared<std::vector<InteractionCandidate>>(std::move(candidates));
                return [job, shared_candidates] {
                    job->controller->apply_candidates(job->generation,
                                                      std::move(*shared_candidates));
                };
            },
            .cancelled =
                [generation, controller = this] {
                    if (controller->state_ && controller->state_->generation == generation) {
                        controller->state_->loading = false;
                        controller->pending_task_ = {};
                    }
                },
            .failed =
                [generation, controller = this](const std::exception_ptr& failure) {
                    controller->apply_failure(generation, failure);
                },
        });
    } catch (const std::exception& exception) {
        state.loading = false;
        state.error = exception.what();
    }
}

void InteractionController::cancel_pending() noexcept {
    if (async_runtime_ != nullptr && pending_task_.valid()) {
        (void)async_runtime_->cancel(pending_task_);
    }
    pending_task_ = {};
}

void InteractionController::apply_candidates(std::uint64_t generation,
                                             std::vector<InteractionCandidate> candidates) {
    if (!state_ || state_->generation != generation) {
        return;
    }
    state_->candidates = std::move(candidates);
    state_->selected = 0;
    state_->loading = false;
    state_->error.clear();
    pending_task_ = {};
}

void InteractionController::apply_failure(std::uint64_t generation,
                                          const std::exception_ptr& failure) {
    if (!state_ || state_->generation != generation) {
        return;
    }
    state_->loading = false;
    pending_task_ = {};
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
        state_->error = "interaction provider failed";
    } catch (const std::exception& exception) {
        state_->error = exception.what();
    } catch (...) {
        state_->error = "interaction provider failed";
    }
}

std::vector<InteractionCandidate>
InteractionController::rank(std::vector<InteractionCandidate> candidates, std::string_view query,
                            const std::stop_token* cancellation) {
    const std::vector<std::string> terms = query_terms(query);
    struct RankedCandidate {
        InteractionCandidate candidate;
        std::size_t score = 0;
    };
    std::vector<RankedCandidate> ranked;
    ranked.reserve(candidates.size());
    for (InteractionCandidate& candidate : candidates) {
        if (cancellation != nullptr && cancellation->stop_requested()) {
            return {};
        }
        const std::string haystack =
            lowercase(candidate.filter_text.empty() ? std::string_view(candidate.label)
                                                    : std::string_view(candidate.filter_text));
        std::size_t score = haystack.size();
        bool matches = true;
        for (const std::string& term : terms) {
            const std::size_t position = haystack.find(term);
            if (position == std::string::npos) {
                matches = false;
                break;
            }
            if (score <= std::numeric_limits<std::size_t>::max() - position) {
                score += position;
            }
        }
        if (matches) {
            ranked.push_back({.candidate = std::move(candidate), .score = score});
        }
    }
    std::ranges::stable_sort(ranked, {}, &RankedCandidate::score);
    std::vector<InteractionCandidate> result;
    result.reserve(ranked.size());
    for (RankedCandidate& candidate : ranked) {
        result.push_back(std::move(candidate.candidate));
    }
    return result;
}

} // namespace cind
