#include "editor/interaction.hpp"

#include "editor/runtime.hpp"

#include <format>
#include <memory>
#include <stdexcept>
#include <utility>

namespace cind {

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

void InteractionProviderRegistry::configure(std::string_view name, Complete complete) {
    if (sealed_) {
        throw std::logic_error("interaction provider registry is sealed");
    }
    if (!complete) {
        throw std::invalid_argument("interaction provider implementation must not be empty");
    }
    const auto provider = providers_.find(std::string(name));
    if (provider == providers_.end()) {
        throw std::out_of_range("unknown interaction provider");
    }
    provider->second = std::move(complete);
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
    const std::optional<KeymapId> keymap = runtime_->keymaps().find(request.keymap);
    if (!keymap) {
        return std::unexpected(std::format("unknown interaction keymap '{}'", request.keymap));
    }
    const std::optional<InputStateId> input_state =
        runtime_->input_states().find(request.input_state);
    if (!input_state) {
        return std::unexpected(
            std::format("unknown interaction input state '{}'", request.input_state));
    }
    if (request.kind == InteractionKind::Picker &&
        (request.provider.empty() || !providers_->contains(request.provider))) {
        return std::unexpected(std::format("unknown interaction provider '{}'", request.provider));
    }
    const CommandTarget origin = state_ ? state_->origin
                                        : CommandTarget{.window = context.window_id(),
                                                        .buffer = context.buffer_id(),
                                                        .view = context.view_id()};
    (void)cancel();
    BufferId buffer;
    ViewId view;
    WindowId window;
    try {
        buffer = runtime_->buffers().create({.name = " *minibuffer*",
                                             .initial_text = request.initial_input,
                                             .kind = BufferKind::Minibuffer,
                                             .resource_uri = std::nullopt,
                                             .read_only = false});
        view = runtime_->views().create(
            buffer, TextOffset{static_cast<std::uint32_t>(request.initial_input.size())});
        runtime_->views().get(view).keymaps().push_back(*keymap);
        runtime_->views().set_base_input_state(view, *input_state);
        window = runtime_->windows().create(view);
    } catch (const std::exception& exception) {
        if (window.valid()) {
            (void)runtime_->windows().erase(window);
        }
        if (view.valid()) {
            (void)runtime_->views().erase(view);
        }
        if (buffer.valid()) {
            (void)runtime_->buffers().erase(buffer);
        }
        return std::unexpected(exception.what());
    }
    state_.emplace(InteractionState{.request = std::move(request),
                                    .origin = origin,
                                    .window = window,
                                    .buffer = buffer,
                                    .view = view,
                                    .candidates = {},
                                    .selected = 0,
                                    .generation = 0,
                                    .history_index = std::nullopt,
                                    .history_draft = {},
                                    .history_navigation_revision = std::nullopt,
                                    .loading = false,
                                    .error = {}});
    refresh();
    return {};
}

std::string InteractionController::input_text() const {
    return state_ ? runtime_->buffers().get(state_->buffer).snapshot().content().to_string()
                  : std::string();
}

TextOffset InteractionController::input_caret() const {
    return state_ ? runtime_->views().caret(state_->view) : TextOffset{};
}

RevisionId InteractionController::input_revision() const {
    return state_ ? runtime_->buffers().get(state_->buffer).snapshot().revision() : 0;
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

bool InteractionController::previous_history() {
    InteractionState* active = state();
    if (active == nullptr || active->request.history.empty()) {
        return false;
    }
    const std::vector<std::string>& entries = history(active->request.history);
    if (entries.empty()) {
        return false;
    }
    if (!active->history_index) {
        active->history_draft = input_text();
        active->history_index = entries.size() - 1;
    } else if (*active->history_index > 0) {
        --*active->history_index;
    } else {
        return false;
    }
    replace_input(entries[*active->history_index]);
    active->history_navigation_revision = input_revision();
    refresh();
    return true;
}

bool InteractionController::next_history() {
    InteractionState* active = state();
    if (active == nullptr || !active->history_index || active->request.history.empty()) {
        return false;
    }
    const std::vector<std::string>& entries = history(active->request.history);
    if (*active->history_index + 1 < entries.size()) {
        ++*active->history_index;
        replace_input(entries[*active->history_index]);
    } else {
        std::string draft = std::move(active->history_draft);
        active->history_index.reset();
        replace_input(draft);
    }
    active->history_navigation_revision = input_revision();
    refresh();
    return true;
}

void InteractionController::refresh_candidates() {
    const bool history_navigation =
        state_ && state_->history_navigation_revision == input_revision();
    refresh(!history_navigation);
}

void InteractionController::replace_input(std::string_view input) {
    InteractionState* active = state();
    if (active == nullptr) {
        return;
    }
    Buffer& buffer = runtime_->buffers().get(active->buffer);
    const TextOffset end{buffer.snapshot().content().size_bytes()};
    EditTransaction transaction = buffer.begin_transaction();
    transaction.replace({TextOffset{}, end}, input);
    (void)transaction.commit();
    runtime_->views().set_caret(active->view, buffer.snapshot().content().end_offset());
}

std::expected<InteractionSubmission, std::string> InteractionController::submit() {
    InteractionState* active = state();
    if (active == nullptr) {
        return std::unexpected("no interaction is active");
    }
    std::string value = input_text();
    if (active->request.kind == InteractionKind::Picker && !active->loading &&
        !active->candidates.empty()) {
        value = active->candidates[active->selected].value;
    } else if (active->request.kind == InteractionKind::Picker &&
               !active->request.allow_custom_input) {
        active->error = active->loading ? "candidates are still loading" : "no matching candidate";
        return std::unexpected(active->error);
    }

    InteractionSubmission submission{
        .accept_command = active->request.accept_command,
        .invocation = {.arguments = std::move(active->request.arguments), .prefix = {}},
        .target = active->origin,
    };
    submission.invocation.arguments.emplace_back(value);
    if (!active->request.history.empty() && !value.empty()) {
        std::vector<std::string>& entries = histories_[active->request.history];
        if (entries.empty() || entries.back() != value) {
            entries.push_back(value);
            constexpr std::size_t maximum_history = 100;
            if (entries.size() > maximum_history) {
                entries.erase(entries.begin());
            }
        }
    }
    cancel_pending();
    const WindowId window = active->window;
    const ViewId view = active->view;
    const BufferId buffer = active->buffer;
    state_.reset();
    destroy_surface(window, view, buffer);
    return submission;
}

bool InteractionController::cancel() noexcept {
    InteractionState* active = state();
    if (active == nullptr) {
        return false;
    }
    cancel_pending();
    const WindowId window = active->window;
    const ViewId view = active->view;
    const BufferId buffer = active->buffer;
    state_.reset();
    destroy_surface(window, view, buffer);
    return true;
}

void InteractionController::destroy_surface(WindowId window, ViewId view,
                                            BufferId buffer) noexcept {
    try {
        (void)runtime_->windows().erase(window);
        (void)runtime_->views().erase(view);
        (void)runtime_->buffers().erase(buffer);
    } catch (...) {
        return;
    }
}

const std::vector<std::string>& InteractionController::history(std::string_view name) const {
    static const std::vector<std::string> empty;
    const auto found = histories_.find(std::string(name));
    return found == histories_.end() ? empty : found->second;
}

void InteractionController::refresh(bool input_edited) {
    InteractionState* active = state();
    if (active == nullptr) {
        return;
    }
    if (input_edited) {
        active->history_index.reset();
        active->history_draft.clear();
        active->history_navigation_revision.reset();
    }
    cancel_pending();
    InteractionState& state = *active;
    const std::uint64_t generation = ++next_generation_;
    state.generation = generation;
    state.loading = false;
    state.error.clear();
    if (state.request.kind != InteractionKind::Picker) {
        state.selected = 0;
        state.candidates.clear();
        return;
    }
    try {
        const std::string query = input_text();
        CommandContext context(*runtime_, state.origin.window, state.origin.buffer,
                               state.origin.view);
        InteractionProviderResult result =
            providers_->complete(state.request.provider, context, query);
        if (auto* candidates = std::get_if<std::vector<InteractionCandidate>>(&result)) {
            state.candidates = std::move(*candidates);
            state.selected = 0;
            return;
        }
        state.loading = true;
        if (auto* asynchronous = std::get_if<InteractionCandidateAsync>(&result)) {
            if (!asynchronous->start) {
                state.loading = false;
                state.error = "async interaction provider has no start operation";
                return;
            }
            auto settled = std::make_shared<bool>(false);
            std::expected<InteractionCandidateAsync::Cancel, std::string> started =
                asynchronous->start(
                    // The provider boundary reports allocation and ranking failures through the
                    // interaction error channel below.
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    [generation, controller = this,
                     settled](std::vector<InteractionCandidate> candidates) {
                        *settled = true;
                        try {
                            controller->apply_candidates(generation, std::move(candidates));
                        } catch (...) {
                            controller->apply_failure(generation, std::current_exception());
                        }
                    },
                    [generation, controller = this, settled](std::string message) {
                        *settled = true;
                        if (controller->state_ && controller->state_->generation == generation) {
                            controller->state_->loading = false;
                            controller->state_->error = std::move(message);
                            controller->cancel_pending_task_ = {};
                        }
                    },
                    [generation, controller = this, settled] {
                        *settled = true;
                        if (controller->state_ && controller->state_->generation == generation) {
                            controller->state_->loading = false;
                            controller->cancel_pending_task_ = {};
                        }
                    });
            if (!started) {
                state.loading = false;
                state.error = std::move(started.error());
                return;
            }
            if (!*settled) {
                cancel_pending_task_ = std::move(*started);
            }
            return;
        }
        if (async_runtime_ == nullptr) {
            state.loading = false;
            state.error = "async interaction provider has no runtime";
            return;
        }
        struct Job {
            std::uint64_t generation = 0;
            InteractionCandidateWork work;
            InteractionController* controller = nullptr;
        };
        auto job =
            std::make_shared<Job>(Job{.generation = generation,
                                      .work = std::move(std::get<InteractionCandidateWork>(result)),
                                      .controller = this});
        const AsyncTaskId task = async_runtime_->submit({
            .work = [job](const std::stop_token& cancellation) -> AsyncCompletion {
                std::vector<InteractionCandidate> candidates = job->work(cancellation);
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
                        controller->cancel_pending_task_ = {};
                    }
                },
            .failed =
                [generation, controller = this](const std::exception_ptr& failure) {
                    controller->apply_failure(generation, failure);
                },
        });
        cancel_pending_task_ = [runtime = async_runtime_, task] { (void)runtime->cancel(task); };
    } catch (const std::exception& exception) {
        state.loading = false;
        state.error = exception.what();
    }
}

void InteractionController::cancel_pending() noexcept {
    if (cancel_pending_task_) {
        try {
            cancel_pending_task_();
            // Cancellation is best-effort; generation checks make late
            // completion inert.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
    }
    cancel_pending_task_ = {};
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
    cancel_pending_task_ = {};
}

void InteractionController::apply_failure(std::uint64_t generation,
                                          const std::exception_ptr& failure) {
    if (!state_ || state_->generation != generation) {
        return;
    }
    state_->loading = false;
    cancel_pending_task_ = {};
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

} // namespace cind
