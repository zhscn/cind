#include "editor/search_commands.hpp"

#include "editor/runtime.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace cind {

SearchCommands::SearchCommands(EditorRuntime& runtime, EditSessionResolver session,
                               MessageSink message_sink)
    : runtime_(&runtime), session_(std::move(session)), message_sink_(std::move(message_sink)) {
    accept_command_ = runtime_->commands().define(
        "search.accept", [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept(context, invocation);
        });
    runtime_->commands().define(
        "search.prompt", [this](CommandContext& context, const CommandInvocation& invocation) {
            (void)context;
            (void)invocation;
            return begin(true);
        });
    runtime_->commands().define(
        "search.backward-prompt",
        [this](CommandContext& context, const CommandInvocation& invocation) {
            (void)context;
            (void)invocation;
            return begin(false);
        });
    runtime_->commands().define(
        "search.next", [this](CommandContext& context, const CommandInvocation&) -> CommandResult {
            (void)move(true, context.view_id());
            return CommandCompleted{};
        });
    runtime_->commands().define(
        "search.previous",
        [this](CommandContext& context, const CommandInvocation&) -> CommandResult {
            (void)move(false, context.view_id());
            return CommandCompleted{};
        });
}

CommandResult SearchCommands::begin(bool forward) const {
    const std::string direction = forward ? "search" : "search backward";
    const std::string prompt = query_.empty() ? std::format("{}: ", direction)
                                              : std::format("{} [{}]: ", direction, query_);
    return InteractionRequest{.kind = InteractionKind::Text,
                              .prompt = prompt,
                              .initial_input = {},
                              .history = "search",
                              .provider = {},
                              .allow_custom_input = true,
                              .accept_command = accept_command_,
                              .arguments = {forward}};
}

CommandResult SearchCommands::accept(CommandContext& context, const CommandInvocation& invocation) {
    if (invocation.arguments.size() < 2 ||
        !std::holds_alternative<bool>(invocation.arguments.front()) ||
        !std::holds_alternative<std::string>(invocation.arguments.back())) {
        return std::unexpected(CommandError{"search accepts a direction and query"});
    }
    const std::string& input = std::get<std::string>(invocation.arguments.back());
    if (!input.empty()) {
        query_ = input;
    }
    (void)move(std::get<bool>(invocation.arguments.front()), context.view_id());
    return CommandCompleted{};
}

bool SearchCommands::move(bool forward, ViewId view) {
    EditSession& active = session_(view);
    if (query_.empty()) {
        message_sink_("search query is empty");
        return false;
    }
    const DocumentSnapshot snapshot = active.snapshot();
    const std::string text = snapshot.content().to_string();
    const std::size_t caret = active.caret().value;
    std::size_t found = std::string::npos;
    bool wrapped = false;
    if (forward) {
        found = text.find(query_, std::min(caret + 1, text.size()));
        if (found == std::string::npos) {
            found = text.find(query_);
            wrapped = found != std::string::npos;
        }
    } else {
        if (caret > 0) {
            found = text.rfind(query_, caret - 1);
        }
        if (found == std::string::npos) {
            found = text.rfind(query_);
            wrapped = found != std::string::npos;
        }
    }
    if (found == std::string::npos) {
        message_sink_(std::format("\"{}\" not found", query_));
        return false;
    }
    active.view().viewport().preferred_column.reset();
    active.set_caret(TextOffset{static_cast<std::uint32_t>(found)});
    message_sink_(wrapped ? "search wrapped" : std::string());
    return true;
}

} // namespace cind
