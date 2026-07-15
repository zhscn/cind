#include "editor/settings.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

namespace cind {

namespace {

bool valid_setting_name(std::string_view name) {
    if (name.empty() || name.front() == '.' || name.back() == '.') {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '.' || ch == '-' || ch == '_';
    });
}

} // namespace

SettingId SettingRegistry::define(std::string name, SettingValue default_value,
                                  SettingScopeMask scopes) {
    if (sealed_) {
        throw std::logic_error("setting registry is sealed");
    }
    if (!valid_setting_name(name)) {
        throw std::invalid_argument("invalid setting name");
    }
    if (scopes == 0 || (scopes & ~kAllSettingScopes) != 0) {
        throw std::invalid_argument("setting has an invalid scope mask");
    }
    if (by_name_.contains(name)) {
        throw std::invalid_argument(std::format("setting '{}' is already defined", name));
    }
    const SettingId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back(Definition{std::move(name), std::move(default_value), scopes});
    by_name_.emplace(definitions_.back().name, id);
    return id;
}

const SettingRegistry::Definition& SettingRegistry::definition(SettingId id) const {
    if (id.value >= definitions_.size()) {
        throw std::out_of_range("unknown setting id");
    }
    return definitions_[id.value];
}

std::optional<SettingId> SettingRegistry::find(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SettingsLayer::set(SettingId id, SettingValue value) {
    if (sealed_) {
        throw std::logic_error("settings layer is sealed");
    }
    const SettingRegistry::Definition& definition = registry_->definition(id);
    if ((definition.scopes & setting_scope_bit(scope_)) == 0) {
        throw std::invalid_argument(
            std::format("setting '{}' is not valid in this scope", definition.name));
    }
    if (definition.default_value.index() != value.index()) {
        throw std::invalid_argument(
            std::format("setting '{}' has an invalid value type", definition.name));
    }
    values_.insert_or_assign(id.value, std::move(value));
}

bool SettingsLayer::erase(SettingId id) {
    if (sealed_) {
        throw std::logic_error("settings layer is sealed");
    }
    registry_->definition(id);
    return values_.erase(id.value) != 0;
}

const SettingValue* SettingsLayer::find(SettingId id) const {
    registry_->definition(id);
    auto it = values_.find(id.value);
    return it == values_.end() ? nullptr : &it->second;
}

SettingsResolver::SettingsResolver(const SettingRegistry& registry,
                                   std::vector<const SettingsLayer*> layers)
    : registry_(&registry), layers_(std::move(layers)) {
    for (const SettingsLayer* layer : layers_) {
        if (layer == nullptr || &layer->registry() != registry_) {
            throw std::invalid_argument("settings resolver contains a foreign layer");
        }
    }
}

const SettingValue& SettingsResolver::get(SettingId id) const {
    for (const SettingsLayer* layer : layers_) {
        if (const SettingValue* value = layer->find(id)) {
            return *value;
        }
    }
    return registry_->definition(id).default_value;
}

} // namespace cind
