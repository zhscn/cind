#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cind {

using SettingValue = std::variant<bool, std::int64_t, double, std::string>;

enum class SettingScope : std::uint8_t {
    Application,
    Workspace,
    Project,
    Language,
    Mode,
    Buffer,
    View,
};

using SettingScopeMask = std::uint32_t;

constexpr SettingScopeMask setting_scope_bit(SettingScope scope) {
    return SettingScopeMask{1} << static_cast<std::uint32_t>(scope);
}

constexpr SettingScopeMask operator|(SettingScope lhs, SettingScope rhs) {
    return setting_scope_bit(lhs) | setting_scope_bit(rhs);
}

constexpr SettingScopeMask operator|(SettingScopeMask lhs, SettingScope rhs) {
    return lhs | setting_scope_bit(rhs);
}

constexpr SettingScopeMask kAllSettingScopes =
    setting_scope_bit(SettingScope::Application) | setting_scope_bit(SettingScope::Workspace) |
    setting_scope_bit(SettingScope::Project) | setting_scope_bit(SettingScope::Language) |
    setting_scope_bit(SettingScope::Mode) | setting_scope_bit(SettingScope::Buffer) |
    setting_scope_bit(SettingScope::View);

struct SettingId {
    std::uint32_t value = 0;
    friend constexpr auto operator<=>(SettingId, SettingId) = default;
};

class SettingRegistry {
public:
    struct Definition {
        std::string name;
        SettingValue default_value;
        SettingScopeMask scopes = 0;
    };

    SettingId define(std::string name, SettingValue default_value,
                     SettingScopeMask scopes = kAllSettingScopes);
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(SettingId id) const;
    std::optional<SettingId> find(std::string_view name) const;
    std::size_t size() const { return definitions_.size(); }

private:
    std::vector<Definition> definitions_;
    std::unordered_map<std::string, SettingId> by_name_;
    bool sealed_ = false;
};

class SettingsLayer {
public:
    SettingsLayer(const SettingRegistry& registry, SettingScope scope)
        : registry_(&registry), scope_(scope) {}

    void set(SettingId id, SettingValue value);
    bool erase(SettingId id);
    const SettingValue* find(SettingId id) const;

    SettingScope scope() const { return scope_; }
    const SettingRegistry& registry() const { return *registry_; }
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

private:
    const SettingRegistry* registry_;
    SettingScope scope_;
    std::unordered_map<std::uint32_t, SettingValue> values_;
    bool sealed_ = false;
};

// Resolves an explicit, caller-supplied scope chain. Layers are ordered from
// most specific to least specific; there is no dynamically bound ambient
// settings environment.
class SettingsResolver {
public:
    SettingsResolver(const SettingRegistry& registry, std::vector<const SettingsLayer*> layers);

    const SettingValue& get(SettingId id) const;

    template <typename T> const T& get_as(SettingId id) const {
        const SettingValue& value = get(id);
        if (const T* typed = std::get_if<T>(&value)) {
            return *typed;
        }
        throw std::logic_error("setting value has an unexpected type");
    }

private:
    const SettingRegistry* registry_;
    std::vector<const SettingsLayer*> layers_;
};

} // namespace cind
