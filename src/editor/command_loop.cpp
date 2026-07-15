#include "editor/command_loop.hpp"

#include "editor/runtime.hpp"

#include <utility>

namespace cind {

void CommandLoop::set_keymaps(std::vector<KeymapId> keymaps) {
    for (const KeymapId keymap : keymaps) {
        (void)runtime_->keymaps().definition(keymap);
    }
    keymaps_ = std::move(keymaps);
    cancel_pending();
}

CommandLoopResult CommandLoop::dispatch(KeyStroke key, CommandContext& context) {
    if (minibuffer_) {
        return {.status = CommandLoopStatus::AwaitingInput,
                .consumed = false,
                .command = std::nullopt,
                .key_sequence = {},
                .message = "minibuffer is awaiting input"};
    }

    const bool continued_sequence = !pending_.empty();
    pending_.push_back(key);
    const std::string sequence_text = format_key_sequence(pending_);
    KeymapMatch match;
    std::optional<KeymapId> matched_keymap = pending_keymap_;
    if (pending_keymap_) {
        match = runtime_->keymaps().resolve(*pending_keymap_, pending_);
    } else {
        for (const KeymapId keymap : keymaps_) {
            match = runtime_->keymaps().resolve(keymap, pending_);
            if (match.kind != KeymapMatchKind::None) {
                matched_keymap = keymap;
                break;
            }
        }
    }

    if (match.kind == KeymapMatchKind::Prefix) {
        pending_keymap_ = matched_keymap;
        return {.status = CommandLoopStatus::Prefix,
                .consumed = true,
                .command = std::nullopt,
                .key_sequence = sequence_text,
                .message = sequence_text + " …"};
    }
    if (match.kind == KeymapMatchKind::None) {
        pending_.clear();
        pending_keymap_.reset();
        repeat_count_.reset();
        return {.status = CommandLoopStatus::NotHandled,
                .consumed = continued_sequence,
                .command = std::nullopt,
                .key_sequence = sequence_text,
                .message = continued_sequence ? "undefined key: " + sequence_text : std::string()};
    }

    pending_.clear();
    pending_keymap_.reset();
    CommandInvocation invocation{.arguments = {}, .repeat_count = repeat_count_};
    repeat_count_.reset();
    return invoke(match.command, context, invocation, sequence_text);
}

void CommandLoop::cancel_pending() {
    pending_.clear();
    pending_keymap_.reset();
    repeat_count_.reset();
}

void CommandLoop::minibuffer_insert(std::string_view text) {
    if (minibuffer_) {
        minibuffer_->input.append(text);
    }
}

bool CommandLoop::minibuffer_erase_backward() {
    if (!minibuffer_ || minibuffer_->input.empty()) {
        return false;
    }
    std::size_t start = minibuffer_->input.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(minibuffer_->input[start]) & 0xC0U) == 0x80U) {
        --start;
    }
    minibuffer_->input.resize(start);
    return true;
}

CommandLoopResult CommandLoop::submit_minibuffer(CommandContext& context) {
    if (!minibuffer_) {
        return {};
    }
    MinibufferState state = std::move(*minibuffer_);
    minibuffer_.reset();
    CommandInvocation invocation{.arguments = std::move(state.request.arguments),
                                 .repeat_count = std::nullopt};
    invocation.arguments.emplace_back(std::move(state.input));
    return invoke(state.request.accept_command, context, invocation, {});
}

CommandLoopResult CommandLoop::cancel_minibuffer() {
    if (!minibuffer_) {
        return {};
    }
    minibuffer_.reset();
    return {.status = CommandLoopStatus::Cancelled,
            .consumed = true,
            .command = std::nullopt,
            .key_sequence = {},
            .message = "cancelled"};
}

CommandLoopResult CommandLoop::invoke(CommandId command, CommandContext& context,
                                      const CommandInvocation& invocation,
                                      std::string key_sequence) {
    const CommandRegistry& commands = runtime_->commands();
    if (!commands.enabled(command, context)) {
        return {.status = CommandLoopStatus::Disabled,
                .consumed = true,
                .command = command,
                .key_sequence = std::move(key_sequence),
                .message = "command is disabled in this context"};
    }
    return finish(command, commands.invoke(command, context, invocation), std::move(key_sequence));
}

CommandLoopResult CommandLoop::finish(CommandId command, CommandResult result,
                                      std::string key_sequence) {
    if (!result) {
        return {.status = CommandLoopStatus::Error,
                .consumed = true,
                .command = command,
                .key_sequence = std::move(key_sequence),
                .message = std::move(result.error().message)};
    }
    if (MinibufferRequest* request = std::get_if<MinibufferRequest>(&*result)) {
        if (!request->accept_command) {
            return {.status = CommandLoopStatus::Error,
                    .consumed = true,
                    .command = command,
                    .key_sequence = std::move(key_sequence),
                    .message = "minibuffer request has no accept command"};
        }
        std::string input = request->initial_input;
        minibuffer_.emplace(
            MinibufferState{.request = std::move(*request), .input = std::move(input)});
        return {.status = CommandLoopStatus::AwaitingInput,
                .consumed = true,
                .command = command,
                .key_sequence = std::move(key_sequence),
                .message = {}};
    }
    return {.status = CommandLoopStatus::Executed,
            .consumed = true,
            .command = command,
            .key_sequence = std::move(key_sequence),
            .message = {}};
}

} // namespace cind
