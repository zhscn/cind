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
                .message = sequence_text + " …",
                .interaction = std::nullopt};
    }
    if (match.kind == KeymapMatchKind::None) {
        pending_.clear();
        pending_keymap_.reset();
        repeat_count_.reset();
        return {.status = CommandLoopStatus::NotHandled,
                .consumed = continued_sequence,
                .command = std::nullopt,
                .key_sequence = sequence_text,
                .message = continued_sequence ? "undefined key: " + sequence_text : std::string(),
                .interaction = std::nullopt};
    }

    pending_.clear();
    pending_keymap_.reset();
    CommandInvocation invocation{.arguments = {}, .repeat_count = repeat_count_};
    repeat_count_.reset();
    return invoke(match.command, context, invocation, sequence_text);
}

CommandLoopResult CommandLoop::execute(CommandId command, CommandContext& context,
                                       const CommandInvocation& invocation) {
    cancel_pending();
    return invoke(command, context, invocation, {});
}

void CommandLoop::cancel_pending() {
    pending_.clear();
    pending_keymap_.reset();
    repeat_count_.reset();
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
                .message = "command is disabled in this context",
                .interaction = std::nullopt};
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
                .message = std::move(result.error().message),
                .interaction = std::nullopt};
    }
    if (InteractionRequest* request = std::get_if<InteractionRequest>(&*result)) {
        if (!request->accept_command) {
            return {.status = CommandLoopStatus::Error,
                    .consumed = true,
                    .command = command,
                    .key_sequence = std::move(key_sequence),
                    .message = "interaction request has no accept command",
                    .interaction = std::nullopt};
        }
        return {.status = CommandLoopStatus::AwaitingInput,
                .consumed = true,
                .command = command,
                .key_sequence = std::move(key_sequence),
                .message = {},
                .interaction = std::move(*request)};
    }
    return {.status = CommandLoopStatus::Executed,
            .consumed = true,
            .command = command,
            .key_sequence = std::move(key_sequence),
            .message = {},
            .interaction = std::nullopt};
}

} // namespace cind
