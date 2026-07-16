#include "editor/keymap.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

std::string uppercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return result;
}

std::expected<KeyStroke, KeyParseError> parse_key_stroke(std::string_view notation) {
    KeyModifiers modifiers;
    while (notation.size() >= 2 && notation[1] == '-') {
        switch (notation.front()) {
        case 'C':
        case 'c':
            modifiers |= KeyModifier::Control;
            break;
        case 'M':
        case 'm':
            modifiers |= KeyModifier::Alt;
            break;
        case 'S':
            modifiers |= KeyModifier::Shift;
            break;
        case 's':
            modifiers |= KeyModifier::Super;
            break;
        default:
            return std::unexpected(
                KeyParseError{std::format("unknown key modifier in '{}'", notation)});
        }
        notation.remove_prefix(2);
    }
    if (notation.empty()) {
        return std::unexpected(KeyParseError{"key is missing after its modifiers"});
    }

    const std::string name = uppercase(notation);
    if (name == "LEFT") {
        return KeyStroke::named(KeyCode::Left, modifiers);
    }
    if (name == "RIGHT") {
        return KeyStroke::named(KeyCode::Right, modifiers);
    }
    if (name == "UP") {
        return KeyStroke::named(KeyCode::Up, modifiers);
    }
    if (name == "DOWN") {
        return KeyStroke::named(KeyCode::Down, modifiers);
    }
    if (name == "HOME") {
        return KeyStroke::named(KeyCode::Home, modifiers);
    }
    if (name == "END") {
        return KeyStroke::named(KeyCode::End, modifiers);
    }
    if (name == "PGUP" || name == "PAGEUP") {
        return KeyStroke::named(KeyCode::PageUp, modifiers);
    }
    if (name == "PGDN" || name == "PAGEDOWN") {
        return KeyStroke::named(KeyCode::PageDown, modifiers);
    }
    if (name == "BACKSPACE" || name == "BS") {
        return KeyStroke::named(KeyCode::Backspace, modifiers);
    }
    if (name == "DELETE" || name == "DEL") {
        return KeyStroke::named(KeyCode::Delete, modifiers);
    }
    if (name == "RET" || name == "ENTER") {
        return KeyStroke::named(KeyCode::Enter, modifiers);
    }
    if (name == "TAB") {
        return KeyStroke::named(KeyCode::Tab, modifiers);
    }
    if (name == "ESC" || name == "ESCAPE") {
        return KeyStroke::named(KeyCode::Escape, modifiers);
    }
    if (name == "SPC" || name == "SPACE") {
        return KeyStroke::character_key(U' ', modifiers);
    }
    if (notation.size() == 1 && static_cast<unsigned char>(notation.front()) < 0x80U) {
        char ch = notation.front();
        if (ch >= 'A' && ch <= 'Z') {
            modifiers |= KeyModifier::Shift;
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return KeyStroke::character_key(static_cast<unsigned char>(ch), modifiers);
    }
    return std::unexpected(
        KeyParseError{std::format("'{}' is not a supported key name", notation)});
}

std::string_view named_key(KeyCode code) {
    switch (code) {
    case KeyCode::Character:
        return {};
    case KeyCode::Left:
        return "Left";
    case KeyCode::Right:
        return "Right";
    case KeyCode::Up:
        return "Up";
    case KeyCode::Down:
        return "Down";
    case KeyCode::Home:
        return "Home";
    case KeyCode::End:
        return "End";
    case KeyCode::PageUp:
        return "PgUp";
    case KeyCode::PageDown:
        return "PgDn";
    case KeyCode::Backspace:
        return "Backspace";
    case KeyCode::Delete:
        return "Delete";
    case KeyCode::Enter:
        return "RET";
    case KeyCode::Tab:
        return "TAB";
    case KeyCode::Escape:
        return "ESC";
    }
    return {};
}

} // namespace

std::expected<KeySequence, KeyParseError> parse_key_sequence(std::string_view notation) {
    KeySequence result;
    std::size_t position = 0;
    while (position < notation.size()) {
        while (position < notation.size() && notation[position] == ' ') {
            ++position;
        }
        if (position == notation.size()) {
            break;
        }
        const std::size_t end = notation.find(' ', position);
        const std::string_view token = notation.substr(position, end - position);
        std::expected<KeyStroke, KeyParseError> key = parse_key_stroke(token);
        if (!key) {
            return std::unexpected(std::move(key.error()));
        }
        result.push_back(*key);
        position = end == std::string_view::npos ? notation.size() : end + 1;
    }
    if (result.empty()) {
        return std::unexpected(KeyParseError{"key sequence must not be empty"});
    }
    return result;
}

std::string format_key_stroke(KeyStroke key) {
    std::string result;
    if (has_modifier(key.modifiers, KeyModifier::Control)) {
        result += "C-";
    }
    if (has_modifier(key.modifiers, KeyModifier::Alt)) {
        result += "M-";
    }
    if (has_modifier(key.modifiers, KeyModifier::Shift)) {
        result += "S-";
    }
    if (has_modifier(key.modifiers, KeyModifier::Super)) {
        result += "s-";
    }
    if (key.code != KeyCode::Character) {
        result += named_key(key.code);
    } else if (key.character == U' ') {
        result += "SPC";
    } else if (key.character >= 0x20 && key.character < 0x7f) {
        result.push_back(static_cast<char>(key.character));
    } else {
        result += std::format("U+{:04X}", static_cast<std::uint32_t>(key.character));
    }
    return result;
}

std::string format_key_sequence(std::span<const KeyStroke> sequence) {
    std::string result;
    for (const KeyStroke key : sequence) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += format_key_stroke(key);
    }
    return result;
}

KeymapId KeymapRegistry::define(std::string name) {
    if (sealed_) {
        throw std::logic_error("keymap registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("keymap name must not be empty");
    }
    if (by_name_.contains(name)) {
        throw std::invalid_argument(std::format("keymap '{}' is already defined", name));
    }
    const KeymapId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back(
        StoredKeymap{.definition = Definition{std::move(name)}, .parent = std::nullopt});
    by_name_.emplace(definitions_.back().definition.name, id);
    return id;
}

void KeymapRegistry::set_parent(KeymapId keymap, std::optional<KeymapId> parent) {
    if (sealed_) {
        throw std::logic_error("keymap registry is sealed");
    }
    StoredKeymap& map = stored(keymap);
    if (parent) {
        (void)stored(*parent);
        for (std::optional<KeymapId> ancestor = parent; ancestor;
             ancestor = stored(*ancestor).parent) {
            if (*ancestor == keymap) {
                throw std::invalid_argument("keymap parent cycle");
            }
        }
    }
    map.parent = parent;
}

void KeymapRegistry::bind(KeymapId keymap, std::span<const KeyStroke> sequence, CommandId command) {
    if (sealed_) {
        throw std::logic_error("keymap registry is sealed");
    }
    if (sequence.empty()) {
        throw std::invalid_argument("key sequence must not be empty");
    }
    if (!command) {
        throw std::invalid_argument("key binding requires a valid command");
    }

    StoredKeymap& map = stored(keymap);
    std::uint32_t node_index = 0;
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        Node& node = map.nodes[node_index];
        if (node.command) {
            throw std::invalid_argument(std::format("'{}' extends an existing command binding",
                                                    format_key_sequence(sequence.first(index))));
        }
        const auto child = std::ranges::find_if(
            node.children, [&](const auto& entry) { return entry.first == sequence[index]; });
        if (child != node.children.end()) {
            node_index = child->second;
            continue;
        }
        if (map.nodes.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("keymap is exhausted");
        }
        const std::uint32_t new_index = static_cast<std::uint32_t>(map.nodes.size());
        map.nodes.push_back({});
        try {
            map.nodes[node_index].children.emplace_back(sequence[index], new_index);
        } catch (...) {
            map.nodes.pop_back();
            throw;
        }
        node_index = new_index;
    }
    Node& terminal = map.nodes[node_index];
    if (!terminal.children.empty()) {
        throw std::invalid_argument(
            std::format("'{}' is already a key sequence prefix", format_key_sequence(sequence)));
    }
    terminal.command = command;
}

void KeymapRegistry::bind(KeymapId keymap, std::string_view notation, CommandId command) {
    std::expected<KeySequence, KeyParseError> sequence = parse_key_sequence(notation);
    if (!sequence) {
        throw std::invalid_argument(sequence.error().message);
    }
    bind(keymap, *sequence, command);
}

const KeymapRegistry::Definition& KeymapRegistry::definition(KeymapId id) const {
    return stored(id).definition;
}

std::optional<KeymapId> KeymapRegistry::parent(KeymapId id) const {
    return stored(id).parent;
}

std::optional<KeymapId> KeymapRegistry::find(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return found == by_name_.end() ? std::nullopt : std::optional(found->second);
}

KeymapMatch KeymapRegistry::resolve(KeymapId keymap, std::span<const KeyStroke> sequence) const {
    for (std::optional<KeymapId> current = keymap; current; current = stored(*current).parent) {
        const StoredKeymap& map = stored(*current);
        std::uint32_t node_index = 0;
        bool found = true;
        for (const KeyStroke key : sequence) {
            const Node& node = map.nodes[node_index];
            const auto child = std::ranges::find_if(
                node.children, [&](const auto& entry) { return entry.first == key; });
            if (child == node.children.end()) {
                found = false;
                break;
            }
            node_index = child->second;
        }
        if (!found) {
            continue;
        }
        const Node& terminal = map.nodes[node_index];
        if (terminal.command) {
            return {.kind = KeymapMatchKind::Command, .command = *terminal.command};
        }
        if (!terminal.children.empty()) {
            return {.kind = KeymapMatchKind::Prefix, .command = {}};
        }
    }
    return {};
}

std::vector<KeymapCompletion> KeymapRegistry::completions(KeymapId keymap,
                                                          std::span<const KeyStroke> prefix) const {
    std::vector<KeymapCompletion> result;
    for (std::optional<KeymapId> current = keymap; current; current = stored(*current).parent) {
        const StoredKeymap& map = stored(*current);
        std::uint32_t node_index = 0;
        bool found = true;
        for (const KeyStroke key : prefix) {
            const Node& node = map.nodes[node_index];
            const auto child = std::ranges::find_if(
                node.children, [&](const auto& entry) { return entry.first == key; });
            if (child == node.children.end()) {
                found = false;
                break;
            }
            node_index = child->second;
        }
        if (!found) {
            continue;
        }

        const Node& node = map.nodes[node_index];
        if (node.command) {
            break;
        }
        for (const auto& [key, child_index] : node.children) {
            if (std::ranges::any_of(result, [key](const KeymapCompletion& completion) {
                    return completion.key == key;
                })) {
                continue;
            }
            const Node& child = map.nodes[child_index];
            result.push_back(
                {.key = key, .command = child.command, .prefix = !child.children.empty()});
        }
    }
    return result;
}

std::vector<KeymapBinding> KeymapRegistry::bindings(KeymapId keymap) const {
    std::vector<KeymapBinding> result;
    for (std::optional<KeymapId> current = keymap; current; current = stored(*current).parent) {
        const StoredKeymap& map = stored(*current);
        KeySequence sequence;
        const auto visit = [&](this const auto& self, std::uint32_t node_index) -> void {
            const Node& node = map.nodes[node_index];
            if (node.command) {
                const KeymapMatch effective = resolve(keymap, sequence);
                if (effective.kind == KeymapMatchKind::Command &&
                    effective.command == *node.command &&
                    std::ranges::none_of(result, [&](const KeymapBinding& binding) {
                        return binding.sequence == sequence;
                    })) {
                    result.push_back({.sequence = sequence, .command = *node.command});
                }
            }
            for (const auto& [key, child_index] : node.children) {
                sequence.push_back(key);
                self(child_index);
                sequence.pop_back();
            }
        };
        visit(0);
    }
    return result;
}

KeymapRegistry::StoredKeymap& KeymapRegistry::stored(KeymapId id) {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown keymap id");
    }
    return definitions_[id.value];
}

const KeymapRegistry::StoredKeymap& KeymapRegistry::stored(KeymapId id) const {
    return const_cast<KeymapRegistry*>(this)->stored(id);
}

} // namespace cind
