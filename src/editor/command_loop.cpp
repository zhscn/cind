#include "editor/command_loop.hpp"

#include "editor/runtime.hpp"

#include <algorithm>
#include <exception>
#include <format>
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
    cancel_key_sequence();
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
    cancel_key_sequence();
}

CommandLoopResult CommandLoop::dispatch(KeyStroke key, CommandContext& context,
                                        CommandPrefix prefix) {
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
                .interaction = std::nullopt,
                .next_prefix = std::move(prefix)};
    }
    if (match.kind == KeymapMatchKind::None) {
        cancel_sequence();
        return {.status = CommandLoopStatus::NotHandled,
                .consumed = continued_sequence,
                .command = std::nullopt,
                .key_sequence = sequence_text,
                .message = continued_sequence ? "undefined key: " + sequence_text : std::string(),
                .interaction = std::nullopt,
                .next_prefix = {}};
    }

    cancel_key_sequence();
    CommandInvocation invocation{.arguments = {}, .prefix = std::move(prefix)};
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
            cancel_sequence();
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
                                       CommandPrefix prefix) {
    cancel_key_sequence();
    return invoke(command, context, {.arguments = {}, .prefix = std::move(prefix)}, {});
}

CommandLoopResult CommandLoop::execute(CommandId command, CommandContext& context,
                                       const CommandInvocation& invocation) {
    cancel_key_sequence();
    return invoke(command, context, invocation, {});
}

void CommandLoop::cancel_key_sequence() {
    pending_.clear();
    pending_keymap_.reset();
}

void CommandLoop::cancel_sequence() {
    cancel_key_sequence();
}

CommandLoopResult CommandLoop::invoke(CommandId command, CommandContext& context,
                                      const CommandInvocation& invocation,
                                      std::string key_sequence) {
    const CommandRegistry& commands = runtime_->commands();
    std::optional<CommandContext> targeted_context;
    CommandContext* active_context = &context;
    RevisionId initial_revision = active_context->buffer().snapshot().revision();
    CommandId current = command;
    CommandInvocation current_invocation = invocation;
    constexpr int maximum_dispatch_depth = 32;
    for (int depth = 0; depth < maximum_dispatch_depth; ++depth) {
        if (!commands.enabled(current, *active_context)) {
            const CommandResult disabled =
                std::unexpected(CommandError{"command is disabled in this context"});
            if (std::optional<std::string> error =
                    apply_selection_result(disabled, *active_context, initial_revision)) {
                return {.status = CommandLoopStatus::Error,
                        .consumed = true,
                        .command = current,
                        .key_sequence = std::move(key_sequence),
                        .message = std::move(*error),
                        .interaction = std::nullopt,
                        .next_prefix = {}};
            }
            return {.status = CommandLoopStatus::Disabled,
                    .consumed = true,
                    .command = current,
                    .key_sequence = std::move(key_sequence),
                    .message = "command is disabled in this context",
                    .interaction = std::nullopt,
                    .next_prefix = {}};
        }
        CommandResult result;
        try {
            result = commands.invoke(current, *active_context, current_invocation);
        } catch (const std::exception& exception) {
            result = std::unexpected(CommandError{exception.what()});
        } catch (...) {
            result = std::unexpected(CommandError{"command failed with an unknown exception"});
        }
        if (!result) {
            return finish(current, std::move(result), std::move(key_sequence), *active_context,
                          initial_revision);
        }
        if (CommandDispatch* dispatch = std::get_if<CommandDispatch>(&*result)) {
            if (dispatch->invocation.prefix.empty()) {
                dispatch->invocation.prefix = current_invocation.prefix;
            }
            current = dispatch->command;
            current_invocation = std::move(dispatch->invocation);
            if (dispatch->target) {
                targeted_context.emplace(*runtime_, dispatch->target->window,
                                         dispatch->target->buffer, dispatch->target->view);
                active_context = &*targeted_context;
                initial_revision = active_context->buffer().snapshot().revision();
            }
            continue;
        }
        return finish(current, std::move(result), std::move(key_sequence), *active_context,
                      initial_revision);
    }
    return finish(current, std::unexpected(CommandError{"command dispatch depth exceeded"}),
                  std::move(key_sequence), *active_context, initial_revision);
}

CommandLoopResult CommandLoop::finish(CommandId command, CommandResult result,
                                      std::string key_sequence, CommandContext& context,
                                      RevisionId initial_revision) {
    if (std::optional<std::string> error =
            apply_selection_result(result, context, initial_revision)) {
        return {.status = CommandLoopStatus::Error,
                .consumed = true,
                .command = command,
                .key_sequence = std::move(key_sequence),
                .message = std::move(*error),
                .interaction = std::nullopt,
                .next_prefix = {}};
    }
    if (result) {
        if (CommandPrefixUpdate* update = std::get_if<CommandPrefixUpdate>(&*result)) {
            if (update->prefix.register_name && update->prefix.register_name->empty()) {
                return {.status = CommandLoopStatus::Error,
                        .consumed = true,
                        .command = command,
                        .key_sequence = std::move(key_sequence),
                        .message = "command prefix register must not be empty",
                        .interaction = std::nullopt,
                        .next_prefix = {}};
            }
            for (std::size_t index = 0; index < update->prefix.extra.size(); ++index) {
                if (update->prefix.extra[index].name.empty()) {
                    return {.status = CommandLoopStatus::Error,
                            .consumed = true,
                            .command = command,
                            .key_sequence = std::move(key_sequence),
                            .message = "command prefix extra name must not be empty",
                            .interaction = std::nullopt,
                            .next_prefix = {}};
                }
                for (std::size_t other = 0; other < index; ++other) {
                    if (update->prefix.extra[other].name == update->prefix.extra[index].name) {
                        return {.status = CommandLoopStatus::Error,
                                .consumed = true,
                                .command = command,
                                .key_sequence = std::move(key_sequence),
                                .message = "command prefix extra names must be unique",
                                .interaction = std::nullopt,
                                .next_prefix = {}};
                    }
                }
            }
            CommandPrefix next_prefix = std::move(update->prefix);
            return {.status = CommandLoopStatus::PrefixArgument,
                    .consumed = true,
                    .command = command,
                    .key_sequence = std::move(key_sequence),
                    .message = format_command_prefix(next_prefix),
                    .interaction = std::nullopt,
                    .next_prefix = std::move(next_prefix)};
        }
    }
    if (!result) {
        return {.status = CommandLoopStatus::Error,
                .consumed = true,
                .command = command,
                .key_sequence = std::move(key_sequence),
                .message = std::move(result.error().message),
                .interaction = std::nullopt,
                .next_prefix = {}};
    }
    if (InteractionRequest* request = std::get_if<InteractionRequest>(&*result)) {
        if (!request->accept_command) {
            return {.status = CommandLoopStatus::Error,
                    .consumed = true,
                    .command = command,
                    .key_sequence = std::move(key_sequence),
                    .message = "interaction request has no accept command",
                    .interaction = std::nullopt,
                    .next_prefix = {}};
        }
        return {.status = CommandLoopStatus::AwaitingInput,
                .consumed = true,
                .command = command,
                .key_sequence = std::move(key_sequence),
                .message = {},
                .interaction = std::move(*request),
                .next_prefix = {}};
    }
    return {.status = CommandLoopStatus::Executed,
            .consumed = true,
            .command = command,
            .key_sequence = std::move(key_sequence),
            .message = {},
            .interaction = std::nullopt,
            .next_prefix = {}};
}

std::optional<std::string> CommandLoop::apply_selection_result(const CommandResult& result,
                                                               CommandContext& context,
                                                               RevisionId initial_revision) {
    View* view = runtime_->views().try_get(context.view_id());
    if (view == nullptr) {
        return std::nullopt;
    }
    const Buffer* buffer = runtime_->buffers().try_get(context.buffer_id());
    const bool edited = buffer != nullptr && buffer->snapshot().revision() != initial_revision;

    const CommandSelectionResult* requested = nullptr;
    if (result) {
        if (const CommandCompleted* completed = std::get_if<CommandCompleted>(&*result)) {
            requested = &completed->selection;
        }
    }
    const CommandSelectionResult default_result = CommandSelectionDefault{};
    if (requested == nullptr) {
        requested = &default_result;
    }

    try {
        if (std::holds_alternative<CommandSelectionPreserve>(*requested)) {
            return std::nullopt;
        }
        if (std::holds_alternative<CommandSelectionCollapse>(*requested)) {
            runtime_->views().clear_selection(context.view_id());
            return std::nullopt;
        }
        if (const ViewSelection* replacement = std::get_if<ViewSelection>(requested)) {
            ViewSelection selection{.ranges = replacement->ranges,
                                    .primary = replacement->primary,
                                    .metadata = replacement->metadata};
            runtime_->views().set_selection(context.view_id(), std::move(selection));
            return std::nullopt;
        }
        if (edited &&
            runtime_->selection_edit_policy(context.view_id()) == SelectionEditPolicy::Collapse) {
            runtime_->views().clear_selection(context.view_id());
        }
    } catch (const std::exception& exception) {
        return std::format("invalid command selection result: {}", exception.what());
    }
    return std::nullopt;
}

} // namespace cind
