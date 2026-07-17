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
    definitions_.push_back(StoredKeymap{.definition = Definition{std::move(name)},
                                        .parent = std::nullopt,
                                        .nodes = {Node{}},
                                        .remaps = {}});
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
        std::vector<bool> visited(definitions_.size(), false);
        if (reaches(*parent, keymap, visited)) {
            throw std::invalid_argument("keymap relationship cycle");
        }
    }
    map.parent = parent;
}

std::uint32_t KeymapRegistry::ensure_path(StoredKeymap& map, std::span<const KeyStroke> sequence) {
    if (sequence.empty()) {
        throw std::invalid_argument("key sequence must not be empty");
    }
    std::uint32_t node_index = 0;
    for (const KeyStroke key : sequence) {
        Node& node = map.nodes[node_index];
        node.command.reset();
        node.prefix_keymap.reset();
        node.label.clear();
        const auto child = std::ranges::find_if(
            node.children, [&](const auto& entry) { return entry.first == key; });
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
            map.nodes[node_index].children.emplace_back(key, new_index);
        } catch (...) {
            map.nodes.pop_back();
            throw;
        }
        node_index = new_index;
    }
    return node_index;
}

void KeymapRegistry::bind(KeymapId keymap, std::span<const KeyStroke> sequence, CommandId command) {
    if (sealed_) {
        throw std::logic_error("keymap registry is sealed");
    }
    if (!command) {
        throw std::invalid_argument("key binding requires a valid command");
    }

    StoredKeymap& map = stored(keymap);
    Node& terminal = map.nodes[ensure_path(map, sequence)];
    terminal.children.clear();
    terminal.prefix_keymap.reset();
    terminal.label.clear();
    terminal.command = command;
}

void KeymapRegistry::bind(KeymapId keymap, std::string_view notation, CommandId command) {
    std::expected<KeySequence, KeyParseError> sequence = parse_key_sequence(notation);
    if (!sequence) {
        throw std::invalid_argument(sequence.error().message);
    }
    bind(keymap, *sequence, command);
}

void KeymapRegistry::bind_prefix(KeymapId keymap, std::span<const KeyStroke> sequence,
                                 KeymapId prefix, std::string label) {
    if (sealed_) {
        throw std::logic_error("keymap registry is sealed");
    }
    (void)stored(keymap);
    (void)stored(prefix);
    std::vector<bool> visited(definitions_.size(), false);
    if (reaches(prefix, keymap, visited)) {
        throw std::invalid_argument("keymap relationship cycle");
    }
    StoredKeymap& map = stored(keymap);
    Node& terminal = map.nodes[ensure_path(map, sequence)];
    terminal.command.reset();
    terminal.children.clear();
    terminal.prefix_keymap = prefix;
    terminal.label = std::move(label);
}

void KeymapRegistry::bind_prefix(KeymapId keymap, std::string_view notation, KeymapId prefix,
                                 std::string label) {
    std::expected<KeySequence, KeyParseError> sequence = parse_key_sequence(notation);
    if (!sequence) {
        throw std::invalid_argument(sequence.error().message);
    }
    bind_prefix(keymap, *sequence, prefix, std::move(label));
}

void KeymapRegistry::bind_remap(KeymapId keymap, CommandId command, CommandId replacement) {
    if (sealed_) {
        throw std::logic_error("keymap registry is sealed");
    }
    if (!command || !replacement) {
        throw std::invalid_argument("key remap requires valid commands");
    }
    StoredKeymap& map = stored(keymap);
    auto existing = std::ranges::find_if(
        map.remaps, [command](const auto& remap) { return remap.first == command; });
    if (existing == map.remaps.end()) {
        map.remaps.emplace_back(command, replacement);
    } else {
        existing->second = replacement;
    }
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

bool KeymapRegistry::reaches(KeymapId from, KeymapId target, std::vector<bool>& visited) const {
    if (from == target) {
        return true;
    }
    if (visited[from.value]) {
        return false;
    }
    visited[from.value] = true;
    const StoredKeymap& map = stored(from);
    if (map.parent && reaches(*map.parent, target, visited)) {
        return true;
    }
    return std::ranges::any_of(map.nodes, [&](const Node& node) {
        return node.prefix_keymap && reaches(*node.prefix_keymap, target, visited);
    });
}

KeymapMatch
KeymapRegistry::resolve_from(KeymapId keymap, std::span<const KeyStroke> sequence,
                             std::size_t offset,
                             std::vector<std::pair<KeymapId, std::size_t>>& active) const {
    if (std::ranges::find(active, std::pair{keymap, offset}) != active.end()) {
        return {};
    }
    active.emplace_back(keymap, offset);
    const auto finish = [&](KeymapMatch match) {
        active.pop_back();
        return match;
    };

    for (std::optional<KeymapId> current = keymap; current; current = stored(*current).parent) {
        const StoredKeymap& map = stored(*current);
        std::uint32_t node_index = 0;
        bool found = true;
        for (std::size_t index = offset; index < sequence.size(); ++index) {
            const Node& node = map.nodes[node_index];
            const auto child = std::ranges::find_if(
                node.children, [&](const auto& entry) { return entry.first == sequence[index]; });
            if (child == node.children.end()) {
                found = false;
                break;
            }
            node_index = child->second;
            const Node& selected = map.nodes[node_index];
            if (selected.prefix_keymap) {
                if (index + 1 == sequence.size()) {
                    return finish(
                        {.kind = KeymapMatchKind::Prefix, .command = {}, .source = current});
                }
                KeymapMatch delegated =
                    resolve_from(*selected.prefix_keymap, sequence, index + 1, active);
                if (delegated.kind != KeymapMatchKind::None) {
                    return finish(delegated);
                }
                found = false;
                break;
            }
        }
        if (!found) {
            continue;
        }
        const Node& terminal = map.nodes[node_index];
        if (terminal.command) {
            return finish({.kind = KeymapMatchKind::Command,
                           .command = *terminal.command,
                           .source = current});
        }
        if (terminal.prefix_keymap || !terminal.children.empty()) {
            return finish({.kind = KeymapMatchKind::Prefix, .command = {}, .source = current});
        }
    }
    return finish({});
}

KeymapMatch KeymapRegistry::resolve(KeymapId keymap, std::span<const KeyStroke> sequence) const {
    std::vector<std::pair<KeymapId, std::size_t>> active;
    return resolve_from(keymap, sequence, 0, active);
}

std::optional<CommandId> KeymapRegistry::remap(KeymapId keymap, CommandId command) const {
    for (std::optional<KeymapId> current = keymap; current; current = stored(*current).parent) {
        const StoredKeymap& map = stored(*current);
        const auto replacement = std::ranges::find_if(
            map.remaps, [command](const auto& entry) { return entry.first == command; });
        if (replacement != map.remaps.end()) {
            return replacement->second;
        }
    }
    return std::nullopt;
}

KeymapMatch KeymapRegistry::resolve(std::span<const KeymapId> layers,
                                    std::span<const KeyStroke> sequence) const {
    KeymapMatch match;
    for (const KeymapId layer : layers) {
        match = resolve(layer, sequence);
        if (match.kind != KeymapMatchKind::None) {
            match.source = layer;
            break;
        }
    }
    if (match.kind != KeymapMatchKind::Command) {
        return match;
    }
    for (const KeymapId layer : layers) {
        if (const std::optional<CommandId> replacement = remap(layer, match.command)) {
            match.command = *replacement;
            break;
        }
    }
    return match;
}

std::vector<KeymapCompletion>
KeymapRegistry::completions_from(KeymapId keymap, std::span<const KeyStroke> prefix,
                                 std::size_t offset,
                                 std::vector<std::pair<KeymapId, std::size_t>>& active) const {
    if (std::ranges::find(active, std::pair{keymap, offset}) != active.end()) {
        return {};
    }
    active.emplace_back(keymap, offset);
    std::vector<KeymapCompletion> result;
    for (std::optional<KeymapId> current = keymap; current; current = stored(*current).parent) {
        const StoredKeymap& map = stored(*current);
        std::uint32_t node_index = 0;
        bool found = true;
        for (std::size_t index = offset; index < prefix.size(); ++index) {
            const Node& node = map.nodes[node_index];
            const auto child = std::ranges::find_if(
                node.children, [&](const auto& entry) { return entry.first == prefix[index]; });
            if (child == node.children.end()) {
                found = false;
                break;
            }
            node_index = child->second;
            const Node& selected = map.nodes[node_index];
            if (selected.prefix_keymap) {
                std::vector<KeymapCompletion> delegated =
                    completions_from(*selected.prefix_keymap, prefix, index + 1, active);
                for (KeymapCompletion& completion : delegated) {
                    if (std::ranges::none_of(result, [&](const KeymapCompletion& existing) {
                            return existing.key == completion.key;
                        })) {
                        result.push_back(std::move(completion));
                    }
                }
                found = false;
                break;
            }
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
            result.push_back({.key = key,
                              .command = child.command,
                              .prefix = child.prefix_keymap || !child.children.empty(),
                              .prefix_keymap = child.prefix_keymap,
                              .label = child.label});
        }
    }
    active.pop_back();
    return result;
}

std::vector<KeymapCompletion> KeymapRegistry::completions(KeymapId keymap,
                                                          std::span<const KeyStroke> prefix) const {
    std::vector<std::pair<KeymapId, std::size_t>> active;
    return completions_from(keymap, prefix, 0, active);
}

std::vector<KeymapCompletion> KeymapRegistry::completions(std::span<const KeymapId> layers,
                                                          std::span<const KeyStroke> prefix) const {
    std::vector<KeymapCompletion> result;
    for (const KeymapId layer : layers) {
        for (KeymapCompletion completion : completions(layer, prefix)) {
            if (std::ranges::any_of(result, [&](const KeymapCompletion& existing) {
                    return existing.key == completion.key;
                })) {
                continue;
            }
            if (completion.command) {
                for (const KeymapId remap_layer : layers) {
                    if (const std::optional<CommandId> replacement =
                            remap(remap_layer, *completion.command)) {
                        completion.command = *replacement;
                        break;
                    }
                }
            }
            result.push_back(std::move(completion));
        }
    }
    return result;
}

std::vector<KeymapEntry> KeymapRegistry::entries(KeymapId keymap) const {
    (void)stored(keymap);
    std::vector<KeymapEntry> result;
    std::vector<KeySequence> pending(1);
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const KeySequence prefix = pending[index];
        for (const KeymapCompletion& completion : completions(keymap, prefix)) {
            KeySequence sequence = prefix;
            sequence.push_back(completion.key);
            if (std::ranges::any_of(
                    result, [&](const KeymapEntry& entry) { return entry.sequence == sequence; })) {
                continue;
            }
            if (completion.command) {
                const KeymapMatch effective = resolve(keymap, sequence);
                if (effective.kind == KeymapMatchKind::Command) {
                    result.push_back({.sequence = std::move(sequence),
                                      .kind = KeymapEntryKind::Command,
                                      .command = effective.command,
                                      .prefix_keymap = std::nullopt,
                                      .label = {}});
                }
                continue;
            }
            if (completion.prefix) {
                result.push_back({.sequence = sequence,
                                  .kind = KeymapEntryKind::Prefix,
                                  .command = std::nullopt,
                                  .prefix_keymap = completion.prefix_keymap,
                                  .label = completion.label});
                pending.push_back(std::move(sequence));
            }
        }
    }
    return result;
}

std::vector<KeymapBinding> KeymapRegistry::bindings(KeymapId keymap) const {
    std::vector<KeymapBinding> result;
    for (KeymapEntry& entry : entries(keymap)) {
        if (entry.kind == KeymapEntryKind::Command && entry.command) {
            result.push_back({.sequence = std::move(entry.sequence), .command = *entry.command});
        }
    }
    return result;
}

std::vector<KeymapRemap> KeymapRegistry::remaps(KeymapId keymap) const {
    std::vector<KeymapRemap> result;
    for (std::optional<KeymapId> current = keymap; current; current = stored(*current).parent) {
        for (const auto& [command, replacement] : stored(*current).remaps) {
            if (std::ranges::none_of(result, [command](const KeymapRemap& remap_entry) {
                    return remap_entry.command == command;
                })) {
                result.push_back({.command = command, .replacement = replacement});
            }
        }
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
