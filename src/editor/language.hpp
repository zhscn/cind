#pragma once

#include "editor/settings.hpp"

#include <array>
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

class LanguageMechanism;

struct LanguageProviderId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(LanguageProviderId, LanguageProviderId) = default;
};

struct LanguageProfileId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(LanguageProfileId, LanguageProfileId) = default;
};

enum class LanguageFacet : std::uint8_t {
    Lexing,
    Syntax,
    Indentation,
    StructuralMotion,
    StructuralEditing,
    Highlighting,
    Completion,
    Formatting,
    Count,
};

using LanguageFacetMask = std::uint32_t;

constexpr LanguageFacetMask language_facet_bit(LanguageFacet facet) {
    return LanguageFacetMask{1} << static_cast<std::uint32_t>(facet);
}

constexpr LanguageFacetMask operator|(LanguageFacet left, LanguageFacet right) {
    return language_facet_bit(left) | language_facet_bit(right);
}

constexpr LanguageFacetMask operator|(LanguageFacetMask left, LanguageFacet right) {
    return left | language_facet_bit(right);
}

constexpr LanguageFacetMask kAllLanguageFacets =
    (LanguageFacetMask{1} << static_cast<std::uint32_t>(LanguageFacet::Count)) - 1;

class LanguageRegistry {
public:
    struct ProviderDefinition {
        std::string name;
        LanguageFacet facet;
        std::shared_ptr<const LanguageMechanism> mechanism;
    };

    struct ProfileDefinition {
        ProfileDefinition(std::string profile_name, const SettingRegistry& settings)
            : name(std::move(profile_name)), defaults(settings, SettingScope::Language) {}

        std::string name;
        std::array<std::optional<LanguageProviderId>,
                   static_cast<std::size_t>(LanguageFacet::Count)>
            providers;
        SettingsLayer defaults;

        std::optional<LanguageProviderId> provider(LanguageFacet facet) const {
            return providers[static_cast<std::size_t>(facet)];
        }
    };

    explicit LanguageRegistry(const SettingRegistry& settings) : settings_(&settings) {}
    LanguageRegistry(const LanguageRegistry& other);
    LanguageRegistry& operator=(const LanguageRegistry& other);

    LanguageProviderId define_provider(std::string name, LanguageFacet facet,
                                       std::shared_ptr<const LanguageMechanism> mechanism);
    LanguageProfileId define_profile(std::string name);
    void clear_profile(LanguageProfileId profile);
    void bind(LanguageProfileId profile, LanguageFacet facet, LanguageProviderId provider);
    void seal();
    bool sealed() const { return sealed_; }

    const ProviderDefinition& provider(LanguageProviderId id) const;
    const ProfileDefinition& profile(LanguageProfileId id) const;
    ProfileDefinition& profile_for_configuration(LanguageProfileId id);
    std::optional<LanguageProviderId> find_provider(std::string_view name) const;
    std::optional<LanguageProfileId> find_profile(std::string_view name) const;

private:
    const SettingRegistry* settings_;
    std::vector<ProviderDefinition> providers_;
    std::vector<std::unique_ptr<ProfileDefinition>> profiles_;
    std::unordered_map<std::string, LanguageProviderId> providers_by_name_;
    std::unordered_map<std::string, LanguageProfileId> profiles_by_name_;
    bool sealed_ = false;
};

} // namespace cind
