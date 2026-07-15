#include "editor/search_commands.hpp"

#include "editor/runtime.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace cind {

SearchCommands::SearchCommands(EditorRuntime& runtime, EditSession& session,
                               MessageSink message_sink)
    : runtime_(&runtime), session_(&session), message_sink_(std::move(message_sink)) {
    accept_command_ = runtime_->commands().define(
        "search.accept", [this](CommandContext& context, const CommandInvocation& invocation) {
            return accept(context, invocation);
        });
    runtime_->commands().define(
        "search.prompt", [this](CommandContext& context, const CommandInvocation& invocation) {
            return begin(context, invocation);
        });
    runtime_->commands().define("search.next",
                                [this](CommandContext&, const CommandInvocation&) -> CommandResult {
                                    (void)move(true);
                                    return CommandCompleted{};
                                });
    runtime_->commands().define("search.previous",
                                [this](CommandContext&, const CommandInvocation&) -> CommandResult {
                                    (void)move(false);
                                    return CommandCompleted{};
                                });
}

CommandResult SearchCommands::begin(CommandContext&, const CommandInvocation&) const {
    const std::string prompt = query_.empty() ? "search: " : std::format("search [{}]: ", query_);
    return MinibufferRequest{.prompt = prompt,
                             .initial_input = {},
                             .history = "search",
                             .completion_provider = "buffer-text",
                             .accept_command = accept_command_,
                             .arguments = {}};
}

CommandResult SearchCommands::accept(CommandContext&, const CommandInvocation& invocation) {
    if (invocation.arguments.empty() ||
        !std::holds_alternative<std::string>(invocation.arguments.back())) {
        return std::unexpected(CommandError{"search accepts one string argument"});
    }
    const std::string& input = std::get<std::string>(invocation.arguments.back());
    if (!input.empty()) {
        query_ = input;
    }
    (void)move(true);
    return CommandCompleted{};
}

bool SearchCommands::move(bool forward) {
    if (query_.empty()) {
        message_sink_("search query is empty");
        return false;
    }
    const DocumentSnapshot snapshot = session_->snapshot();
    const std::string text = snapshot.content().to_string();
    const std::size_t caret = session_->caret().value;
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
    session_->view().viewport().preferred_column.reset();
    session_->set_caret(TextOffset{static_cast<std::uint32_t>(found)});
    message_sink_(wrapped ? "search wrapped" : std::string());
    return true;
}

} // namespace cind
