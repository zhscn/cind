#pragma once

#include "editor/ids.hpp"
#include "editor/settings.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

class Buffer;
class EditorRuntime;
class Project;
class View;

struct CommandId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(CommandId, CommandId) = default;
};

struct CommandInvocation {
    std::vector<SettingValue> arguments;
    std::optional<std::int64_t> repeat_count;
};

struct CommandError {
    std::string message;
};

using CommandResult = std::expected<std::optional<SettingValue>, CommandError>;

class CommandContext {
public:
    CommandContext(EditorRuntime& runtime, BufferId buffer, ViewId view)
        : runtime_(&runtime), buffer_id_(buffer), view_id_(view) {}

    EditorRuntime& runtime() const { return *runtime_; }
    BufferId buffer_id() const { return buffer_id_; }
    ViewId view_id() const { return view_id_; }
    std::optional<ProjectId> project_id() const;
    Buffer& buffer() const;
    Project* project() const;
    View& view() const;
    SettingsResolver settings() const;

private:
    EditorRuntime* runtime_;
    BufferId buffer_id_;
    ViewId view_id_;
};

class CommandRegistry {
public:
    using Execute = std::function<CommandResult(CommandContext&, const CommandInvocation&)>;
    using Enabled = std::function<bool(const CommandContext&)>;

    struct Definition {
        std::string name;
        Execute execute;
        Enabled enabled;
    };

    CommandId define(std::string name, Execute execute, Enabled enabled = {});
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(CommandId id) const;
    std::optional<CommandId> find(std::string_view name) const;
    bool enabled(CommandId id, const CommandContext& context) const;
    CommandResult invoke(CommandId id, CommandContext& context,
                         const CommandInvocation& invocation = {}) const;

private:
    std::vector<Definition> definitions_;
    std::unordered_map<std::string, CommandId> by_name_;
    bool sealed_ = false;
};

} // namespace cind
