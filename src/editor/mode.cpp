#include "editor/mode.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cind {

ModeId ModeRegistry::define(std::string name, ModeKind kind,
                            std::optional<LanguageProfileId> language) {
    if (sealed_) {
        throw std::logic_error("mode registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("mode name must not be empty");
    }
    if (by_name_.contains(name)) {
        throw std::invalid_argument(std::format("mode '{}' is already defined", name));
    }
    if (kind == ModeKind::Minor && language) {
        throw std::invalid_argument("minor modes cannot replace the buffer language profile");
    }
    if (language) {
        languages_->profile(*language);
    }
    const ModeId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back(
        std::make_unique<Definition>(std::move(name), kind, language, *settings_));
    by_name_.emplace(definitions_.back()->name, id);
    return id;
}

void ModeRegistry::set_parent(ModeId mode, std::optional<ModeId> parent) {
    Definition& definition = definition_for_configuration(mode);
    if (parent) {
        const Definition& parent_definition = this->definition(*parent);
        if (parent_definition.kind != definition.kind) {
            throw std::invalid_argument("mode parent must have the same kind");
        }
        if (*parent == mode || reaches(*parent, definition)) {
            throw std::invalid_argument("mode parent relationship cycle");
        }
    }
    if (!definition.keymaps.empty() && definition.derived_keymap_parent) {
        if (keymaps_->parent(definition.keymaps.front()) == definition.derived_keymap_parent) {
            keymaps_->set_parent(definition.keymaps.front(), std::nullopt);
        }
        definition.derived_keymap_parent.reset();
    }
    definition.parent = parent;
    if (!definition.keymaps.empty() &&
        keymaps_->parent(definition.keymaps.front()) == std::nullopt) {
        const std::vector<KeymapId> inherited =
            parent ? effective_keymaps(*parent) : std::vector<KeymapId>{};
        if (!inherited.empty()) {
            keymaps_->set_parent(definition.keymaps.front(), inherited.front());
            definition.derived_keymap_parent = inherited.front();
        }
    }
}

void ModeRegistry::set_interaction_class(ModeId mode,
                                         std::optional<InteractionClass> interaction_class) {
    definition_for_configuration(mode).interaction_class = interaction_class;
}

void ModeRegistry::set_initial_state(ModeId mode, std::optional<InputStateId> state) {
    if (state) {
        (void)input_states_->definition(*state);
    }
    definition_for_configuration(mode).initial_state = state;
}

void ModeRegistry::set_things(ModeId mode, std::vector<ModeThingBinding> things) {
    std::unordered_set<std::string> names;
    for (const ModeThingBinding& thing : things) {
        if (thing.name.empty() || thing.kind.empty()) {
            throw std::invalid_argument("mode thing binding requires a name and kind");
        }
        if (!names.insert(thing.name).second) {
            throw std::invalid_argument(
                std::format("duplicate mode thing binding '{}'", thing.name));
        }
    }
    definition_for_configuration(mode).things = std::move(things);
}

void ModeRegistry::add_keymap(ModeId mode, KeymapId keymap) {
    Definition& definition = definition_for_configuration(mode);
    (void)keymaps_->definition(keymap);
    if (std::ranges::find(definition.keymaps, keymap) != definition.keymaps.end()) {
        return;
    }
    if (definition.keymaps.empty() && keymaps_->parent(keymap) == std::nullopt &&
        definition.parent) {
        const std::vector<KeymapId> inherited = effective_keymaps(*definition.parent);
        if (!inherited.empty()) {
            keymaps_->set_parent(keymap, inherited.front());
            definition.derived_keymap_parent = inherited.front();
        }
    }
    definition.keymaps.push_back(keymap);
}

void ModeRegistry::clear_keymaps(ModeId mode) {
    Definition& definition = definition_for_configuration(mode);
    if (!definition.keymaps.empty() && definition.derived_keymap_parent &&
        keymaps_->parent(definition.keymaps.front()) == definition.derived_keymap_parent) {
        keymaps_->set_parent(definition.keymaps.front(), std::nullopt);
    }
    definition.derived_keymap_parent.reset();
    definition.keymaps.clear();
}

std::vector<KeymapId> ModeRegistry::effective_keymaps(ModeId mode) const {
    const Definition* current = &definition(mode);
    while (current != nullptr) {
        if (!current->keymaps.empty()) {
            return current->keymaps;
        }
        current = current->parent ? &definition(*current->parent) : nullptr;
    }
    return {};
}

bool ModeRegistry::reaches(ModeId from, const Definition& target) const {
    std::optional<ModeId> current = from;
    while (current) {
        const Definition& current_definition = definition(*current);
        if (&current_definition == &target) {
            return true;
        }
        current = current_definition.parent;
    }
    return false;
}

std::optional<InteractionClass> ModeRegistry::inherited_interaction_class(ModeId mode) const {
    const Definition* current = &definition(mode);
    while (current != nullptr) {
        if (current->interaction_class) {
            return current->interaction_class;
        }
        current = current->parent ? &definition(*current->parent) : nullptr;
    }
    return std::nullopt;
}

std::optional<InputStateId> ModeRegistry::inherited_initial_state(ModeId mode) const {
    const Definition* current = &definition(mode);
    while (current != nullptr) {
        if (current->initial_state) {
            return current->initial_state;
        }
        current = current->parent ? &definition(*current->parent) : nullptr;
    }
    return std::nullopt;
}

void ModeRegistry::append_inherited_things(ModeId mode,
                                           std::vector<ModeThingBinding>& things) const {
    const Definition* current = &definition(mode);
    while (current != nullptr) {
        for (const ModeThingBinding& thing : current->things) {
            const bool present = std::ranges::any_of(things, [&](const ModeThingBinding& existing) {
                return existing.name == thing.name;
            });
            if (!present) {
                things.push_back(thing);
            }
        }
        current = current->parent ? &definition(*current->parent) : nullptr;
    }
}

EffectiveModePolicy ModeRegistry::effective_policy(const BufferModes& modes) const {
    EffectiveModePolicy result;
    for (auto mode = modes.minors().rbegin(); mode != modes.minors().rend(); ++mode) {
        if (const std::optional<InteractionClass> interaction =
                inherited_interaction_class(*mode)) {
            result.interaction_class = *interaction;
            break;
        }
    }
    if (modes.major()) {
        const bool minor_overrides = std::ranges::any_of(modes.minors(), [&](ModeId mode) {
            return inherited_interaction_class(mode).has_value();
        });
        if (!minor_overrides) {
            result.interaction_class =
                inherited_interaction_class(*modes.major()).value_or(InteractionClass::Editing);
        }
    }

    for (auto mode = modes.minors().rbegin(); mode != modes.minors().rend(); ++mode) {
        if (const std::optional<InputStateId> initial = inherited_initial_state(*mode)) {
            result.initial_state = initial;
            break;
        }
    }
    if (!result.initial_state && modes.major()) {
        result.initial_state = inherited_initial_state(*modes.major());
    }
    if (!result.initial_state) {
        result.initial_state = interaction_class_state(result.interaction_class);
    }

    for (auto mode = modes.minors().rbegin(); mode != modes.minors().rend(); ++mode) {
        append_inherited_things(*mode, result.things);
    }
    if (modes.major()) {
        append_inherited_things(*modes.major(), result.things);
    }
    return result;
}

void ModeRegistry::set_interaction_class_state(InteractionClass interaction_class,
                                               std::optional<InputStateId> state) {
    if (state) {
        (void)input_states_->definition(*state);
    }
    (interaction_class == InteractionClass::Editing ? editing_state_ : interface_state_) = state;
}

std::optional<InputStateId>
ModeRegistry::interaction_class_state(InteractionClass interaction_class) const {
    return interaction_class == InteractionClass::Editing ? editing_state_ : interface_state_;
}

ModeRegistry::ListenerId ModeRegistry::subscribe(Listener listener) {
    if (!listener) {
        throw std::invalid_argument("mode policy listener must be callable");
    }
    if (next_listener_ == 0) {
        throw std::overflow_error("mode policy listener registry is exhausted");
    }
    const ListenerId id = next_listener_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

bool ModeRegistry::unsubscribe(ListenerId listener) {
    const auto found = std::ranges::find_if(
        listeners_, [listener](const auto& entry) { return entry.first == listener; });
    if (found == listeners_.end()) {
        return false;
    }
    listeners_.erase(found);
    return true;
}

void ModeRegistry::publish(const BufferModePolicyChange& change) const {
    std::vector<Listener> listeners;
    listeners.reserve(listeners_.size());
    for (const auto& [id, listener] : listeners_) {
        (void)id;
        listeners.push_back(listener);
    }
    for (const Listener& listener : listeners) {
        listener(change);
    }
}

void ModeRegistry::seal() {
    if (sealed_) {
        return;
    }
    for (const auto& definition : definitions_) {
        definition->defaults.seal();
    }
    sealed_ = true;
}

const ModeRegistry::Definition& ModeRegistry::definition(ModeId id) const {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown mode id");
    }
    return *definitions_[id.value];
}

ModeRegistry::Definition& ModeRegistry::definition_for_configuration(ModeId id) {
    if (sealed_) {
        throw std::logic_error("mode registry is sealed");
    }
    return const_cast<Definition&>(std::as_const(*this).definition(id));
}

std::optional<ModeId> ModeRegistry::find(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void BufferModes::set_major(const ModeRegistry& registry, std::optional<ModeId> mode) {
    if (&registry != registry_) {
        throw std::invalid_argument("buffer modes belong to a different mode registry");
    }
    if (mode && registry.definition(*mode).kind != ModeKind::Major) {
        throw std::invalid_argument("minor mode cannot be installed as the major mode");
    }
    if (major_ == mode) {
        return;
    }
    EffectiveModePolicy before = registry_->effective_policy(*this);
    major_ = mode;
    publish_if_changed(BufferModeChangeKind::Major, mode, std::move(before));
}

bool BufferModes::enable_minor(const ModeRegistry& registry, ModeId mode) {
    if (&registry != registry_) {
        throw std::invalid_argument("buffer modes belong to a different mode registry");
    }
    if (registry.definition(mode).kind != ModeKind::Minor) {
        throw std::invalid_argument("major mode cannot be enabled as a minor mode");
    }
    if (minor_enabled(mode)) {
        return false;
    }
    EffectiveModePolicy before = registry_->effective_policy(*this);
    minors_.push_back(mode);
    publish_if_changed(BufferModeChangeKind::MinorEnabled, mode, std::move(before));
    return true;
}

bool BufferModes::disable_minor(ModeId mode) {
    auto it = std::ranges::find(minors_, mode);
    if (it == minors_.end()) {
        return false;
    }
    EffectiveModePolicy before = registry_->effective_policy(*this);
    minors_.erase(it);
    publish_if_changed(BufferModeChangeKind::MinorDisabled, mode, std::move(before));
    return true;
}

bool BufferModes::minor_enabled(ModeId mode) const {
    return std::ranges::find(minors_, mode) != minors_.end();
}

void BufferModes::publish_if_changed(BufferModeChangeKind kind, std::optional<ModeId> mode,
                                     EffectiveModePolicy before) {
    EffectiveModePolicy after = registry_->effective_policy(*this);
    if (before != after) {
        registry_->publish({.buffer = buffer_,
                            .kind = kind,
                            .mode = mode,
                            .before = std::move(before),
                            .after = std::move(after)});
    }
}

} // namespace cind
