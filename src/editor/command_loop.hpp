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
};

struct MinibufferState {
    MinibufferRequest request;
    std::string input;
};

// One input focus owns one command loop. Keymap layers are ordered from most
// specific to least specific; the first layer that recognizes a sequence
// decides whether it is a prefix or a complete command.
class CommandLoop {
public:
    explicit CommandLoop(EditorRuntime& runtime) : runtime_(&runtime) {}

    void set_keymaps(std::vector<KeymapId> keymaps);
    std::span<const KeymapId> keymaps() const { return keymaps_; }

    CommandLoopResult dispatch(KeyStroke key, CommandContext& context);
    void cancel_pending();
    std::span<const KeyStroke> pending_sequence() const { return pending_; }
    std::string pending_sequence_text() const { return format_key_sequence(pending_); }
    std::optional<KeymapId> pending_keymap() const { return pending_keymap_; }

    void set_repeat_count(std::optional<std::int64_t> count) { repeat_count_ = count; }
    std::optional<std::int64_t> repeat_count() const { return repeat_count_; }

    bool minibuffer_active() const { return minibuffer_.has_value(); }
    const MinibufferState* minibuffer() const { return minibuffer_ ? &*minibuffer_ : nullptr; }
    void minibuffer_insert(std::string_view text);
    bool minibuffer_erase_backward();
    CommandLoopResult submit_minibuffer(CommandContext& context);
    CommandLoopResult cancel_minibuffer();

private:
    CommandLoopResult invoke(CommandId command, CommandContext& context,
                             const CommandInvocation& invocation, std::string key_sequence);
    CommandLoopResult finish(CommandId command, CommandResult result, std::string key_sequence);

    EditorRuntime* runtime_;
    std::vector<KeymapId> keymaps_;
    KeySequence pending_;
    std::optional<KeymapId> pending_keymap_;
    std::optional<std::int64_t> repeat_count_;
    std::optional<MinibufferState> minibuffer_;
};

} // namespace cind
