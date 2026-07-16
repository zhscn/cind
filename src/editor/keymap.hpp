#pragma once

#include "editor/command.hpp"

#include <compare>
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
};

struct KeymapCompletion {
    KeyStroke key;
    std::optional<CommandId> command;
    bool prefix = false;
};

struct KeymapBinding {
    KeySequence sequence;
    CommandId command;
};

class KeymapRegistry {
public:
    struct Definition {
        std::string name;
    };

    KeymapId define(std::string name);
    void bind(KeymapId keymap, std::span<const KeyStroke> sequence, CommandId command);
    void bind(KeymapId keymap, std::string_view notation, CommandId command);

    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(KeymapId id) const;
    std::optional<KeymapId> find(std::string_view name) const;
    KeymapMatch resolve(KeymapId keymap, std::span<const KeyStroke> sequence) const;
    std::vector<KeymapCompletion> completions(KeymapId keymap,
                                              std::span<const KeyStroke> prefix) const;
    std::vector<KeymapBinding> bindings(KeymapId keymap) const;

private:
    struct Node {
        std::optional<CommandId> command;
        std::vector<std::pair<KeyStroke, std::uint32_t>> children;
    };

    struct StoredKeymap {
        Definition definition;
        std::vector<Node> nodes = {Node{}};
    };

    StoredKeymap& stored(KeymapId id);
    const StoredKeymap& stored(KeymapId id) const;

    std::vector<StoredKeymap> definitions_;
    std::unordered_map<std::string, KeymapId> by_name_;
    bool sealed_ = false;
};

} // namespace cind
