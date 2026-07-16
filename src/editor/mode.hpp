#pragma once

#include "editor/ids.hpp"
#include "editor/language.hpp"
#include "editor/settings.hpp"

#include <compare>
#include <cstdint>
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
        SettingsLayer defaults;
        std::vector<KeymapId> keymaps;
    };

    ModeRegistry(const SettingRegistry& settings, const LanguageRegistry& languages)
        : settings_(&settings), languages_(&languages) {}

    ModeId define(std::string name, ModeKind kind,
                  std::optional<LanguageProfileId> language = std::nullopt);
    void seal();
    bool sealed() const { return sealed_; }

    const Definition& definition(ModeId id) const;
    Definition& definition_for_configuration(ModeId id);
    std::optional<ModeId> find(std::string_view name) const;

private:
    const SettingRegistry* settings_;
    const LanguageRegistry* languages_;
    std::vector<std::unique_ptr<Definition>> definitions_;
    std::unordered_map<std::string, ModeId> by_name_;
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
    std::optional<ModeId> major_;
    std::vector<ModeId> minors_;
};

} // namespace cind
