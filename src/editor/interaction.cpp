#include "editor/interaction.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
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
        while (position < query.size() && std::isspace(static_cast<unsigned char>(query[position]))) {
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

std::vector<InteractionCandidate>
InteractionProviderRegistry::complete(std::string_view name, CommandContext& context,
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
    std::string input = request.initial_input;
    state_.emplace(InteractionState{.request = std::move(request),
                                    .input = std::move(input),
                                    .candidates = {},
                                    .selected = 0,
                                    .generation = 0,
                                    .error = {}});
    refresh(context);
    return {};
}

void InteractionController::insert(std::string_view text, CommandContext& context) {
    if (!state_ || text.empty()) {
        return;
    }
    state_->input.append(text);
    refresh(context);
}

bool InteractionController::erase_backward(CommandContext& context) {
    if (!state_ || state_->input.empty()) {
        return false;
    }
    std::size_t start = state_->input.size() - 1;
    while (start > 0 &&
           (static_cast<unsigned char>(state_->input[start]) & 0xC0U) == 0x80U) {
        --start;
    }
    state_->input.resize(start);
    refresh(context);
    return true;
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
    std::string value = state_->input;
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
    state_.reset();
    return submission;
}

bool InteractionController::cancel() {
    if (!state_) {
        return false;
    }
    state_.reset();
    return true;
}

const std::vector<std::string>& InteractionController::history(std::string_view name) const {
    static const std::vector<std::string> empty;
    const auto found = histories_.find(std::string(name));
    return found == histories_.end() ? empty : found->second;
}

void InteractionController::refresh(CommandContext& context) {
    if (!state_) {
        return;
    }
    ++state_->generation;
    state_->selected = 0;
    state_->error.clear();
    state_->candidates.clear();
    if (state_->request.kind != InteractionKind::Picker) {
        return;
    }
    try {
        state_->candidates = rank(
            providers_->complete(state_->request.provider, context, state_->input), state_->input);
    } catch (const std::exception& exception) {
        state_->error = exception.what();
    }
}

std::vector<InteractionCandidate>
InteractionController::rank(std::vector<InteractionCandidate> candidates, std::string_view query) {
    const std::vector<std::string> terms = query_terms(query);
    struct RankedCandidate {
        InteractionCandidate candidate;
        std::size_t score = 0;
    };
    std::vector<RankedCandidate> ranked;
    ranked.reserve(candidates.size());
    for (InteractionCandidate& candidate : candidates) {
        const std::string haystack = lowercase(candidate.filter_text.empty()
                                                    ? std::string_view(candidate.label)
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
