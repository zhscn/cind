#include "editor/command.hpp"

#include "editor/runtime.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

Buffer& CommandContext::buffer() const {
    return runtime_->buffers().get(buffer_id_);
}

std::optional<ProjectId> CommandContext::project_id() const {
    return buffer().project_id();
}

Project* CommandContext::project() const {
    const std::optional<ProjectId> id = project_id();
    return id ? &runtime_->projects().get(*id) : nullptr;
}

View& CommandContext::view() const {
    return runtime_->views().get(view_id_);
}

SettingsResolver CommandContext::settings() const {
    return runtime_->settings_for(buffer_id_, view_id_);
}

CommandId CommandRegistry::define(std::string name, Execute execute, Enabled enabled) {
    if (sealed_) {
        throw std::logic_error("command registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("command name must not be empty");
    }
    if (!execute) {
        throw std::invalid_argument("command implementation must not be empty");
    }
    if (by_name_.contains(name)) {
        throw std::invalid_argument(std::format("command '{}' is already defined", name));
    }
    const CommandId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back(Definition{std::move(name), std::move(execute), std::move(enabled)});
    by_name_.emplace(definitions_.back().name, id);
    return id;
}

const CommandRegistry::Definition& CommandRegistry::definition(CommandId id) const {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown command id");
    }
    return definitions_[id.value];
}

std::optional<CommandId> CommandRegistry::find(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool CommandRegistry::enabled(CommandId id, const CommandContext& context) const {
    const Definition& command = definition(id);
    return !command.enabled || command.enabled(context);
}

CommandResult CommandRegistry::invoke(CommandId id, CommandContext& context,
                                      const CommandInvocation& invocation) const {
    const Definition& command = definition(id);
    if (command.enabled && !command.enabled(context)) {
        return std::unexpected(CommandError{"command is disabled in this context"});
    }
    return command.execute(context, invocation);
}

} // namespace cind
