#pragma once

#include "editor/keymap.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

class EditorRuntime;

enum class CommandLoopStatus : std::uint8_t {
    NotHandled,
    Prefix,
    Executed,
    AwaitingInput,
    Disabled,
    Cancelled,
    Error,
};

struct CommandLoopResult {
    CommandLoopStatus status = CommandLoopStatus::NotHandled;
    bool consumed = false;
    std::optional<CommandId> command;
    std::string key_sequence;
    std::string message;
    std::optional<InteractionRequest> interaction;
};

struct KeymapLayer {
    KeymapId keymap;
    std::string scope;
};

// One input focus owns one command loop. Keymap layers are ordered from most
// specific to least specific. Lookup evaluates the complete pending sequence
// against every layer on each stroke, so sparse prefixes can fall through
// without losing layer precedence.
class CommandLoop {
public:
    explicit CommandLoop(EditorRuntime& runtime) : runtime_(&runtime) {}

    void set_keymap_layers(std::vector<KeymapLayer> layers);
    std::span<const KeymapLayer> keymap_layers() const { return keymap_layers_; }
    void set_override_keymaps(std::vector<KeymapId> keymaps);
    std::span<const KeymapId> override_keymaps() const { return override_keymaps_; }

    CommandLoopResult dispatch(KeyStroke key, CommandContext& context);
    CommandLoopResult execute(CommandId command, CommandContext& context,
                              const CommandInvocation& invocation = {});
    void cancel_pending();
    std::span<const KeyStroke> pending_sequence() const { return pending_; }
    std::string pending_sequence_text() const { return format_key_sequence(pending_); }
    std::optional<KeymapId> pending_keymap() const { return pending_keymap_; }
    std::vector<KeymapCompletion> pending_completions() const;

    void set_repeat_count(std::optional<std::int64_t> count) { repeat_count_ = count; }
    std::optional<std::int64_t> repeat_count() const { return repeat_count_; }

private:
    CommandLoopResult invoke(CommandId command, CommandContext& context,
                             const CommandInvocation& invocation, std::string key_sequence);
    CommandLoopResult finish(CommandId command, CommandResult result, std::string key_sequence);

    EditorRuntime* runtime_;
    std::vector<KeymapId> override_keymaps_;
    std::vector<KeymapLayer> keymap_layers_;
    KeySequence pending_;
    std::optional<KeymapId> pending_keymap_;
    std::optional<std::int64_t> repeat_count_;
};

} // namespace cind
