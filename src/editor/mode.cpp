#include "editor/mode.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
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
    if (mode && registry.definition(*mode).kind != ModeKind::Major) {
        throw std::invalid_argument("minor mode cannot be installed as the major mode");
    }
    major_ = mode;
}

bool BufferModes::enable_minor(const ModeRegistry& registry, ModeId mode) {
    if (registry.definition(mode).kind != ModeKind::Minor) {
        throw std::invalid_argument("major mode cannot be enabled as a minor mode");
    }
    if (minor_enabled(mode)) {
        return false;
    }
    minors_.push_back(mode);
    return true;
}

bool BufferModes::disable_minor(ModeId mode) {
    auto it = std::ranges::find(minors_, mode);
    if (it == minors_.end()) {
        return false;
    }
    minors_.erase(it);
    return true;
}

bool BufferModes::minor_enabled(ModeId mode) const {
    return std::ranges::find(minors_, mode) != minors_.end();
}

} // namespace cind
