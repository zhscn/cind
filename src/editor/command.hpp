#pragma once

#include "editor/ids.hpp"
#include "editor/selection.hpp"
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
#include <variant>
#include <vector>

namespace cind {

class Buffer;
class EditorRuntime;
class Project;
class View;
class Window;

struct CommandId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(CommandId, CommandId) = default;
};

struct CommandPrefixExtra {
    std::string name;
    SettingValue value;

    friend bool operator==(const CommandPrefixExtra&, const CommandPrefixExtra&) = default;
};

struct CommandPrefix {
    std::optional<std::int64_t> count;
    std::optional<std::string> register_name;
    std::vector<CommandPrefixExtra> extra;

    bool empty() const { return !count && !register_name && extra.empty(); }
    friend bool operator==(const CommandPrefix&, const CommandPrefix&) = default;
};

struct CommandInvocation {
    std::vector<SettingValue> arguments;
    CommandPrefix prefix;
};

std::string format_command_prefix(const CommandPrefix& prefix);

struct CommandError {
    std::string message;
};

struct CommandSelectionDefault {
    friend constexpr bool operator==(CommandSelectionDefault, CommandSelectionDefault) = default;
};

struct CommandSelectionPreserve {
    friend constexpr bool operator==(CommandSelectionPreserve, CommandSelectionPreserve) = default;
};

struct CommandSelectionCollapse {
    friend constexpr bool operator==(CommandSelectionCollapse, CommandSelectionCollapse) = default;
};

// A completed command either delegates edit-time selection handling to the
// active input strategy, preserves or collapses the current selection, or
// installs a complete replacement model.
using CommandSelectionResult = std::variant<CommandSelectionDefault, CommandSelectionPreserve,
                                            CommandSelectionCollapse, ViewSelection>;

struct CommandCompleted {
    std::optional<SettingValue> value = std::nullopt;
    CommandSelectionResult selection = CommandSelectionDefault{};
};

// An explicit command target keeps deferred command composition independent
// of whichever surface currently owns keyboard focus.
struct CommandTarget {
    WindowId window;
    BufferId buffer;
    ViewId view;

    friend bool operator==(const CommandTarget&, const CommandTarget&) = default;
};

// Requests another named command without retaining a callback continuation.
// CommandLoop follows the dispatch and reports the final command as the one
// that executed, which keeps interaction submission and scripted command
// composition inside the same observable command pipeline.
struct CommandDispatch {
    CommandId command;
    CommandInvocation invocation;
    std::optional<CommandTarget> target = std::nullopt;
};

// Prefix commands replace the command loop's pending prefix slot. The next
// ordinary command receives this value in its invocation and consumes it.
struct CommandPrefixUpdate {
    CommandPrefix prefix;
};

enum class InteractionKind : std::uint8_t {
    Text,
    Picker,
};

// Commands request interactive input as data rather than retaining a
// language- or frontend-specific continuation. The accept command receives
// `arguments` followed by the submitted string and may request another
// interaction.
struct InteractionRequest {
    InteractionKind kind = InteractionKind::Text;
    std::string prompt;
    std::string initial_input;
    std::string history;
    std::string provider;
    bool allow_custom_input = true;
    CommandId accept_command;
    std::vector<SettingValue> arguments;
};

using CommandAction =
    std::variant<CommandCompleted, InteractionRequest, CommandDispatch, CommandPrefixUpdate>;
using CommandResult = std::expected<CommandAction, CommandError>;

class CommandContext {
public:
    CommandContext(EditorRuntime& runtime, WindowId window, BufferId buffer, ViewId view);

    EditorRuntime& runtime() const { return *runtime_; }
    WindowId window_id() const { return window_id_; }
    BufferId buffer_id() const { return buffer_id_; }
    ViewId view_id() const { return view_id_; }
    std::optional<ProjectId> project_id() const;
    Buffer& buffer() const;
    Project* project() const;
    View& view() const;
    Window& window() const;
    SettingsResolver settings() const;

private:
    EditorRuntime* runtime_;
    WindowId window_id_;
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
        std::string documentation;
        std::string source;
    };

    CommandId define(std::string name, Execute execute, Enabled enabled = {});
    void configure(CommandId id, Execute execute, Enabled enabled = {});
    void describe(CommandId id, std::string documentation, std::string source);
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(CommandId id) const;
    std::vector<CommandId> all() const;
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
