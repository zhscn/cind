#pragma once

#include "editor/command.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

enum class KeyCode : std::uint8_t {
    Character,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Backspace,
    Delete,
    Enter,
    Tab,
    Escape,
};

enum class KeyModifier : std::uint8_t {
    None = 0,
    Control = 1U << 0U,
    Alt = 1U << 1U,
    Shift = 1U << 2U,
    Super = 1U << 3U,
};

struct KeyModifiers {
    std::uint8_t bits = 0;

    constexpr KeyModifiers() = default;
    constexpr KeyModifiers(KeyModifier modifier) : bits(static_cast<std::uint8_t>(modifier)) {}

    constexpr KeyModifiers& operator|=(KeyModifier modifier) {
        bits |= static_cast<std::uint8_t>(modifier);
        return *this;
    }

    friend constexpr auto operator<=>(KeyModifiers, KeyModifiers) = default;
};

constexpr bool has_modifier(KeyModifiers modifiers, KeyModifier expected) {
    return (modifiers.bits & static_cast<std::uint8_t>(expected)) != 0;
}

struct KeyStroke {
    KeyCode code = KeyCode::Character;
    char32_t character = U'\0';
    KeyModifiers modifiers;

    static constexpr KeyStroke character_key(char32_t character, KeyModifiers modifiers = {}) {
        return {.code = KeyCode::Character, .character = character, .modifiers = modifiers};
    }

    static constexpr KeyStroke named(KeyCode code, KeyModifiers modifiers = {}) {
        return {.code = code, .character = U'\0', .modifiers = modifiers};
    }

    friend constexpr auto operator<=>(KeyStroke, KeyStroke) = default;
};

using KeySequence = std::vector<KeyStroke>;

struct KeyParseError {
    std::string message;
};

std::expected<KeySequence, KeyParseError> parse_key_sequence(std::string_view notation);
std::string format_key_stroke(KeyStroke key);
std::string format_key_sequence(std::span<const KeyStroke> sequence);

enum class KeymapMatchKind : std::uint8_t { None, Prefix, Command };

struct KeymapMatch {
    KeymapMatchKind kind = KeymapMatchKind::None;
    CommandId command;
    std::optional<KeymapId> source;
};

struct KeymapCompletion {
    KeyStroke key;
    std::optional<CommandId> command;
    bool prefix = false;
    std::optional<KeymapId> prefix_keymap;
    std::string label;
};

struct KeymapBinding {
    KeySequence sequence;
    CommandId command;
};

struct KeymapRemap {
    CommandId command;
    CommandId replacement;

    friend constexpr bool operator==(const KeymapRemap&, const KeymapRemap&) = default;
};

enum class KeymapEntryKind : std::uint8_t {
    Command,
    Prefix,
};

struct KeymapEntry {
    KeySequence sequence;
    KeymapEntryKind kind = KeymapEntryKind::Prefix;
    std::optional<CommandId> command;
    std::optional<KeymapId> prefix_keymap;
    std::string label;
};

class KeymapRegistry {
public:
    struct Definition {
        std::string name;
    };

    KeymapId define(std::string name);
    void set_parent(KeymapId keymap, std::optional<KeymapId> parent);
    void bind(KeymapId keymap, std::span<const KeyStroke> sequence, CommandId command);
    void bind(KeymapId keymap, std::string_view notation, CommandId command);
    void bind_prefix(KeymapId keymap, std::span<const KeyStroke> sequence, KeymapId prefix,
                     std::string label = {});
    void bind_prefix(KeymapId keymap, std::string_view notation, KeymapId prefix,
                     std::string label = {});
    void bind_remap(KeymapId keymap, CommandId command, CommandId replacement);

    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(KeymapId id) const;
    std::optional<KeymapId> parent(KeymapId id) const;
    std::optional<KeymapId> find(std::string_view name) const;
    KeymapMatch resolve(KeymapId keymap, std::span<const KeyStroke> sequence) const;
    KeymapMatch resolve(std::span<const KeymapId> layers,
                        std::span<const KeyStroke> sequence) const;
    std::optional<CommandId> remap(KeymapId keymap, CommandId command) const;
    std::vector<KeymapCompletion> completions(KeymapId keymap,
                                              std::span<const KeyStroke> prefix) const;
    std::vector<KeymapBinding> bindings(KeymapId keymap) const;
    std::vector<KeymapEntry> entries(KeymapId keymap) const;
    std::vector<KeymapRemap> remaps(KeymapId keymap) const;

private:
    struct Node {
        std::optional<CommandId> command;
        std::optional<KeymapId> prefix_keymap;
        std::string label;
        std::vector<std::pair<KeyStroke, std::uint32_t>> children;
    };

    struct StoredKeymap {
        Definition definition;
        std::optional<KeymapId> parent;
        std::vector<Node> nodes = {Node{}};
        std::vector<std::pair<CommandId, CommandId>> remaps;
    };

    std::uint32_t ensure_path(StoredKeymap& map, std::span<const KeyStroke> sequence);
    bool reaches(KeymapId from, KeymapId target, std::vector<bool>& visited) const;
    KeymapMatch resolve_from(KeymapId keymap, std::span<const KeyStroke> sequence,
                             std::size_t offset,
                             std::vector<std::pair<KeymapId, std::size_t>>& active) const;
    std::vector<KeymapCompletion>
    completions_from(KeymapId keymap, std::span<const KeyStroke> prefix, std::size_t offset,
                     std::vector<std::pair<KeymapId, std::size_t>>& active) const;
    StoredKeymap& stored(KeymapId id);
    const StoredKeymap& stored(KeymapId id) const;

    std::vector<StoredKeymap> definitions_;
    std::unordered_map<std::string, KeymapId> by_name_;
    bool sealed_ = false;
};

} // namespace cind
