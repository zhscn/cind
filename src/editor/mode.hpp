#pragma once

#include "editor/ids.hpp"
#include "editor/input_state.hpp"
#include "editor/language.hpp"
#include "editor/settings.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

struct ModeId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(ModeId, ModeId) = default;
};

enum class ModeKind : std::uint8_t {
    Major,
    Minor,
};

enum class InteractionClass : std::uint8_t {
    Editing,
    Interface,
};

struct ModeThingBinding {
    std::string name;
    std::string definition;

    friend bool operator==(const ModeThingBinding&, const ModeThingBinding&) = default;
};

struct EffectiveModePolicy {
    InteractionClass interaction_class = InteractionClass::Editing;
    std::optional<InputStateId> initial_state;
    std::vector<ModeThingBinding> things;

    friend bool operator==(const EffectiveModePolicy&, const EffectiveModePolicy&) = default;
};

enum class BufferModeChangeKind : std::uint8_t {
    Major,
    MinorEnabled,
    MinorDisabled,
};

struct BufferModePolicyChange {
    BufferId buffer;
    BufferModeChangeKind kind = BufferModeChangeKind::Major;
    std::optional<ModeId> mode;
    EffectiveModePolicy before;
    EffectiveModePolicy after;
};

class BufferModes;

class ModeRegistry {
public:
    struct Definition {
        Definition(std::string mode_name, ModeKind mode_kind,
                   std::optional<LanguageProfileId> language_profile,
                   const SettingRegistry& settings)
            : name(std::move(mode_name)), kind(mode_kind), language(language_profile),
              defaults(settings, SettingScope::Mode) {}

        std::string name;
        ModeKind kind;
        std::optional<LanguageProfileId> language;
        std::optional<ModeId> parent;
        std::optional<InteractionClass> interaction_class;
        std::optional<InputStateId> initial_state;
        std::vector<ModeThingBinding> things;
        SettingsLayer defaults;
        std::vector<KeymapId> keymaps;
        std::optional<KeymapId> derived_keymap_parent;
    };

    using ListenerId = std::uint64_t;
    using Listener = std::function<void(const BufferModePolicyChange&)>;

    ModeRegistry(const SettingRegistry& settings, const LanguageRegistry& languages,
                 KeymapRegistry& keymaps, const InputStateRegistry& input_states)
        : settings_(&settings), languages_(&languages), keymaps_(&keymaps),
          input_states_(&input_states) {}
    ModeRegistry(const ModeRegistry& other);
    ModeRegistry& operator=(const ModeRegistry& other);

    ModeId define(std::string name, ModeKind kind,
                  std::optional<LanguageProfileId> language = std::nullopt);
    void set_parent(ModeId mode, std::optional<ModeId> parent);
    void set_interaction_class(ModeId mode, std::optional<InteractionClass> interaction_class);
    void set_initial_state(ModeId mode, std::optional<InputStateId> state);
    void set_things(ModeId mode, std::vector<ModeThingBinding> things);
    void add_keymap(ModeId mode, KeymapId keymap);
    void clear_keymaps(ModeId mode);
    std::vector<KeymapId> effective_keymaps(ModeId mode) const;

    EffectiveModePolicy effective_policy(const BufferModes& modes) const;

    ListenerId subscribe(Listener listener);
    bool unsubscribe(ListenerId listener);
    void seal();
    bool sealed() const { return sealed_; }

    const Definition& definition(ModeId id) const;
    Definition& definition_for_configuration(ModeId id);
    std::optional<ModeId> find(std::string_view name) const;

private:
    friend class BufferModes;

    void publish(const BufferModePolicyChange& change) const;
    bool reaches(ModeId from, const Definition& target) const;
    std::optional<InteractionClass> inherited_interaction_class(ModeId mode) const;
    std::optional<InputStateId> inherited_initial_state(ModeId mode) const;
    void append_inherited_things(ModeId mode, std::vector<ModeThingBinding>& things) const;

    const SettingRegistry* settings_;
    const LanguageRegistry* languages_;
    KeymapRegistry* keymaps_;
    const InputStateRegistry* input_states_;
    std::vector<std::unique_ptr<Definition>> definitions_;
    std::unordered_map<std::string, ModeId> by_name_;
    std::vector<std::pair<ListenerId, Listener>> listeners_;
    ListenerId next_listener_ = 1;
    bool sealed_ = false;
};

class BufferModes {
public:
    std::optional<ModeId> major() const { return major_; }
    const std::vector<ModeId>& minors() const { return minors_; }

    void set_major(const ModeRegistry& registry, std::optional<ModeId> mode);
    bool enable_minor(const ModeRegistry& registry, ModeId mode);
    bool disable_minor(ModeId mode);
    bool minor_enabled(ModeId mode) const;

private:
    friend class Buffer;

    BufferModes(BufferId buffer, ModeRegistry& registry) : buffer_(buffer), registry_(&registry) {}
    void publish_if_changed(BufferModeChangeKind kind, std::optional<ModeId> mode,
                            EffectiveModePolicy before);

    BufferId buffer_;
    ModeRegistry* registry_ = nullptr;
    std::optional<ModeId> major_;
    std::vector<ModeId> minors_;
};

} // namespace cind
