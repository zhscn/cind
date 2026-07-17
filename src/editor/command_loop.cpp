#include "editor/command_loop.hpp"

#include "editor/runtime.hpp"

#include <algorithm>
#include <exception>
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
        for (const KeymapEntry& entry : runtime_->keymaps().entries(keymap)) {
            if (entry.kind != KeymapEntryKind::Command || entry.sequence.size() != 1) {
                throw std::invalid_argument("override keymaps require single-key bindings");
            }
        }
    }
    override_keymaps_ = std::move(keymaps);
    cancel_pending();
}

CommandLoopResult CommandLoop::dispatch(KeyStroke key, CommandContext& context) {
    if (std::optional<CommandLoopResult> override = dispatch_override(key, context)) {
        return std::move(*override);
    }

    const bool continued_sequence = !pending_.empty();
    pending_.push_back(key);
    const std::string sequence_text = format_key_sequence(pending_);
    std::vector<KeymapId> layers;
    layers.reserve(keymap_layers_.size());
    for (const KeymapLayer& layer : keymap_layers_) {
        layers.push_back(layer.keymap);
    }
    const KeymapMatch match = runtime_->keymaps().resolve(layers, pending_);

    if (match.kind == KeymapMatchKind::Prefix) {
        pending_keymap_ = match.source;
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

std::optional<CommandLoopResult> CommandLoop::dispatch_override(KeyStroke key,
                                                                CommandContext& context) {
    const KeySequence single{key};
    std::vector<KeymapId> active_keymaps = override_keymaps_;
    active_keymaps.reserve(override_keymaps_.size() + keymap_layers_.size());
    for (const KeymapLayer& layer : keymap_layers_) {
        active_keymaps.push_back(layer.keymap);
    }
    for (const KeymapId keymap : override_keymaps_) {
        KeymapMatch override = runtime_->keymaps().resolve(keymap, single);
        if (override.kind == KeymapMatchKind::Command) {
            for (const KeymapId active : active_keymaps) {
                if (const std::optional<CommandId> replacement =
                        runtime_->keymaps().remap(active, override.command)) {
                    override.command = *replacement;
                    break;
                }
            }
            const std::string sequence_text = format_key_stroke(key);
            cancel_pending();
            return invoke(override.command, context, {}, sequence_text);
        }
    }
    return std::nullopt;
}

std::vector<KeymapCompletion> CommandLoop::pending_completions() const {
    if (pending_.empty()) {
        return {};
    }
    std::vector<KeymapId> layers;
    layers.reserve(keymap_layers_.size());
    for (const KeymapLayer& layer : keymap_layers_) {
        layers.push_back(layer.keymap);
    }
    return runtime_->keymaps().completions(layers, pending_);
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
        CommandResult result;
        try {
            result = commands.invoke(current, context, current_invocation);
        } catch (const std::exception& exception) {
            result = std::unexpected(CommandError{exception.what()});
        } catch (...) {
            result = std::unexpected(CommandError{"command failed with an unknown exception"});
        }
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
