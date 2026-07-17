#include "editor/command.hpp"

#include "editor/runtime.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

std::string format_prefix_value(const SettingValue& value) {
    if (const bool* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "true" : "false";
    }
    if (const std::int64_t* integer = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*integer);
    }
    if (const double* real = std::get_if<double>(&value)) {
        return std::format("{}", *real);
    }
    return std::get<std::string>(value);
}

} // namespace

std::string format_command_prefix(const CommandPrefix& prefix) {
    std::string result;
    const auto append = [&](std::string value) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += std::move(value);
    };
    if (prefix.count) {
        append(std::to_string(*prefix.count));
    }
    if (prefix.register_name) {
        append(std::format("\"{}", *prefix.register_name));
    }
    for (const CommandPrefixExtra& extra : prefix.extra) {
        append(std::format("{}={}", extra.name, format_prefix_value(extra.value)));
    }
    return result;
}

CommandContext::CommandContext(EditorRuntime& runtime, WindowId window, BufferId buffer,
                               ViewId view)
    : runtime_(&runtime), window_id_(window), buffer_id_(buffer), view_id_(view) {
    if (runtime.windows().get(window).view_id() != view) {
        throw std::invalid_argument("command context view is not displayed by its window");
    }
    if (runtime.views().get(view).buffer_id() != buffer) {
        throw std::invalid_argument("command context buffer is not displayed by its view");
    }
}

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

Window& CommandContext::window() const {
    return runtime_->windows().get(window_id_);
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

void CommandRegistry::configure(CommandId id, Execute execute, Enabled enabled) {
    if (sealed_) {
        throw std::logic_error("command registry is sealed");
    }
    Definition& existing = definitions_.at(id.value);
    existing.execute = std::move(execute);
    existing.enabled = std::move(enabled);
}

const CommandRegistry::Definition& CommandRegistry::definition(CommandId id) const {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown command id");
    }
    return definitions_[id.value];
}

std::vector<CommandId> CommandRegistry::all() const {
    std::vector<CommandId> result;
    result.reserve(definitions_.size());
    for (std::uint32_t value = 0; value < definitions_.size(); ++value) {
        result.push_back(CommandId{value});
    }
    return result;
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
