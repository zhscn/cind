#include "editor/language.hpp"

#include "editor/language_mechanism.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

LanguageRegistry::LanguageRegistry(const LanguageRegistry& other)
    : settings_(other.settings_), providers_(other.providers_),
      providers_by_name_(other.providers_by_name_), profiles_by_name_(other.profiles_by_name_),
      sealed_(other.sealed_) {
    profiles_.reserve(other.profiles_.size());
    for (const std::unique_ptr<ProfileDefinition>& profile : other.profiles_) {
        profiles_.push_back(std::make_unique<ProfileDefinition>(*profile));
    }
}

LanguageRegistry& LanguageRegistry::operator=(const LanguageRegistry& other) {
    if (this == &other) {
        return *this;
    }
    settings_ = other.settings_;
    providers_ = other.providers_;
    profiles_.clear();
    profiles_.reserve(other.profiles_.size());
    for (const std::unique_ptr<ProfileDefinition>& profile : other.profiles_) {
        profiles_.push_back(std::make_unique<ProfileDefinition>(*profile));
    }
    providers_by_name_ = other.providers_by_name_;
    profiles_by_name_ = other.profiles_by_name_;
    sealed_ = other.sealed_;
    return *this;
}

LanguageProviderId
LanguageRegistry::define_provider(std::string name, LanguageFacet facet,
                                  std::shared_ptr<const LanguageMechanism> mechanism) {
    if (sealed_) {
        throw std::logic_error("language registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("language provider name must not be empty");
    }
    if (facet == LanguageFacet::Count) {
        throw std::invalid_argument("invalid language facet");
    }
    if (!mechanism || !mechanism->supports(facet)) {
        throw std::invalid_argument("language provider requires a mechanism for its facet");
    }
    if (providers_by_name_.contains(name)) {
        throw std::invalid_argument(std::format("language provider '{}' is already defined", name));
    }
    const LanguageProviderId id{static_cast<std::uint32_t>(providers_.size())};
    providers_.push_back(ProviderDefinition{std::move(name), facet, std::move(mechanism)});
    providers_by_name_.emplace(providers_.back().name, id);
    return id;
}

LanguageProfileId LanguageRegistry::define_profile(std::string name) {
    if (sealed_) {
        throw std::logic_error("language registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("language profile name must not be empty");
    }
    if (profiles_by_name_.contains(name)) {
        throw std::invalid_argument(std::format("language profile '{}' is already defined", name));
    }
    const LanguageProfileId id{static_cast<std::uint32_t>(profiles_.size())};
    profiles_.push_back(std::make_unique<ProfileDefinition>(std::move(name), *settings_));
    profiles_by_name_.emplace(profiles_.back()->name, id);
    return id;
}

void LanguageRegistry::clear_profile(LanguageProfileId profile_id) {
    ProfileDefinition& definition = profile_for_configuration(profile_id);
    definition.providers = {};
    definition.defaults = SettingsLayer(*settings_, SettingScope::Language);
}

void LanguageRegistry::bind(LanguageProfileId profile_id, LanguageFacet facet,
                            LanguageProviderId provider_id) {
    if (sealed_) {
        throw std::logic_error("language registry is sealed");
    }
    const ProviderDefinition& provider_definition = provider(provider_id);
    if (provider_definition.facet != facet) {
        throw std::invalid_argument("language provider has the wrong facet");
    }
    profile_for_configuration(profile_id).providers[static_cast<std::size_t>(facet)] = provider_id;
}

void LanguageRegistry::seal() {
    if (sealed_) {
        return;
    }
    for (const auto& profile_definition : profiles_) {
        profile_definition->defaults.seal();
    }
    sealed_ = true;
}

const LanguageRegistry::ProviderDefinition&
LanguageRegistry::provider(LanguageProviderId id) const {
    if (!id.valid() || id.value >= providers_.size()) {
        throw std::out_of_range("unknown language provider id");
    }
    return providers_[id.value];
}

const LanguageRegistry::ProfileDefinition& LanguageRegistry::profile(LanguageProfileId id) const {
    if (!id.valid() || id.value >= profiles_.size()) {
        throw std::out_of_range("unknown language profile id");
    }
    return *profiles_[id.value];
}

LanguageRegistry::ProfileDefinition&
LanguageRegistry::profile_for_configuration(LanguageProfileId id) {
    if (sealed_) {
        throw std::logic_error("language registry is sealed");
    }
    return const_cast<ProfileDefinition&>(std::as_const(*this).profile(id));
}

std::optional<LanguageProviderId> LanguageRegistry::find_provider(std::string_view name) const {
    auto it = providers_by_name_.find(std::string(name));
    return it == providers_by_name_.end() ? std::nullopt
                                          : std::optional<LanguageProviderId>(it->second);
}

std::optional<LanguageProfileId> LanguageRegistry::find_profile(std::string_view name) const {
    auto it = profiles_by_name_.find(std::string(name));
    return it == profiles_by_name_.end() ? std::nullopt
                                         : std::optional<LanguageProfileId>(it->second);
}

} // namespace cind
