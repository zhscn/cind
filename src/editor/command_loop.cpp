#include "editor/command_loop.hpp"

#include "editor/runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cind {

void CommandLoop::set_keymap_layers(std::vector<KeymapLayer> layers) {
    for (const KeymapLayer& layer : layers) {
        (void)runtime_->keymaps().definition(layer.keymap);
        if (layer.scope.empty()) {
            throw std::invalid_argument("keymap layer requires a scope");
        }
    }
    keymap_layers_ = std::move(layers);
    cancel_pending();
}

void CommandLoop::set_override_keymaps(std::vector<KeymapId> keymaps) {
    for (const KeymapId keymap : keymaps) {
        (void)runtime_->keymaps().definition(keymap);
        for (const KeymapBinding& binding : runtime_->keymaps().bindings(keymap)) {
            if (binding.sequence.size() != 1) {
                throw std::invalid_argument("override keymaps require single-key bindings");
            }
        }
    }
    override_keymaps_ = std::move(keymaps);
    cancel_pending();
}

CommandLoopResult CommandLoop::dispatch(KeyStroke key, CommandContext& context) {
    const KeySequence single{key};
    for (const KeymapId keymap : override_keymaps_) {
        const KeymapMatch override = runtime_->keymaps().resolve(keymap, single);
        if (override.kind == KeymapMatchKind::Command) {
            const std::string sequence_text = format_key_stroke(key);
            cancel_pending();
            return invoke(override.command, context, {}, sequence_text);
        }
    }

    const bool continued_sequence = !pending_.empty();
    pending_.push_back(key);
    const std::string sequence_text = format_key_sequence(pending_);
    KeymapMatch match;
    std::optional<KeymapId> matched_keymap;
    for (const KeymapLayer& layer : keymap_layers_) {
        match = runtime_->keymaps().resolve(layer.keymap, pending_);
        if (match.kind != KeymapMatchKind::None) {
            matched_keymap = layer.keymap;
            break;
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

std::vector<KeymapCompletion> CommandLoop::pending_completions() const {
    if (pending_.empty()) {
        return {};
    }
    std::vector<KeymapCompletion> result;
    for (const KeymapLayer& layer : keymap_layers_) {
        for (const KeymapCompletion& completion :
             runtime_->keymaps().completions(layer.keymap, pending_)) {
            if (std::ranges::any_of(result, [&](const KeymapCompletion& existing) {
                    return existing.key == completion.key;
                })) {
                continue;
            }
            result.push_back(completion);
        }
    }
    return result;
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
    CommandId current = command;
    CommandInvocation current_invocation = invocation;
    constexpr int maximum_dispatch_depth = 32;
    for (int depth = 0; depth < maximum_dispatch_depth; ++depth) {
        if (!commands.enabled(current, context)) {
            return {.status = CommandLoopStatus::Disabled,
                    .consumed = true,
                    .command = current,
                    .key_sequence = std::move(key_sequence),
                    .message = "command is disabled in this context",
                    .interaction = std::nullopt};
        }
        CommandResult result = commands.invoke(current, context, current_invocation);
        if (!result) {
            return finish(current, std::move(result), std::move(key_sequence));
        }
        if (CommandDispatch* dispatch = std::get_if<CommandDispatch>(&*result)) {
            current = dispatch->command;
            current_invocation = std::move(dispatch->invocation);
            continue;
        }
        return finish(current, std::move(result), std::move(key_sequence));
    }
    return {.status = CommandLoopStatus::Error,
            .consumed = true,
            .command = current,
            .key_sequence = std::move(key_sequence),
            .message = "command dispatch depth exceeded",
            .interaction = std::nullopt};
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
