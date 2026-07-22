#include "editor/runtime.hpp"

#include <stdexcept>
#include <vector>

namespace cind {

EditorRuntime::ExtensionCheckpoint::ExtensionCheckpoint(const EditorRuntime& runtime)
    : commands(runtime.commands_), keymaps(runtime.keymaps_), input_states(runtime.input_states_),
      input_strategies(runtime.input_strategies_), things(runtime.things_),
      motions(runtime.motions_), languages(runtime.languages_), modes(runtime.modes_),
      resource_policies(runtime.resource_policies_),
      interaction_providers(runtime.interaction_providers_),
      extensions_sealed(runtime.extensions_sealed_) {}

EditorRuntime::EditorRuntime()
    : application_settings_(setting_definitions_, SettingScope::Application),
      languages_(setting_definitions_), input_states_(keymaps_), input_strategies_(input_states_),
      modes_(setting_definitions_, languages_, keymaps_, input_states_), resource_policies_(modes_),
      buffers_(setting_definitions_, modes_), projects_(buffers_, setting_definitions_),
      views_(buffers_, setting_definitions_, input_states_, input_strategies_, modes_),
      windows_(views_) {}

void EditorRuntime::seal_extensions() {
    if (extensions_sealed_) {
        return;
    }
    setting_definitions_.seal();
    languages_.seal();
    modes_.seal();
    resource_policies_.seal();
    commands_.seal();
    keymaps_.seal();
    input_states_.seal();
    input_strategies_.seal();
    things_.seal();
    motions_.seal();
    interaction_providers_.seal();
    extensions_sealed_ = true;
}

void EditorRuntime::append_mode_layers(std::vector<const SettingsLayer*>& layers,
                                       ModeId mode) const {
    const ModeRegistry::Definition& definition = modes_.definition(mode);
    layers.push_back(&definition.defaults);
    if (definition.language) {
        layers.push_back(&languages_.profile(*definition.language).defaults);
    }
    if (definition.parent) {
        append_mode_layers(layers, *definition.parent);
    }
}

SettingsResolver EditorRuntime::settings_for(BufferId buffer_id, ViewId view_id,
                                             std::optional<ProjectId> project) const {
    const Buffer& buffer = buffers_.get(buffer_id);
    const View& view = views_.get(view_id);
    if (view.buffer_id() != buffer_id) {
        throw std::invalid_argument("view does not display the requested buffer");
    }

    std::vector<const SettingsLayer*> layers;
    layers.push_back(&view.settings());
    layers.push_back(&buffer.settings());

    // The buffer-to-project association lives in Guile, so the caller supplies
    // it rather than this layer reading it back.
    if (project) {
        layers.push_back(&projects_.get(*project).settings());
    }

    // A workspace layer slots between Project and Application when UI session
    // ownership is introduced. Application is explicit user policy;
    // mode values are defaults rather than hidden buffer-local mutation.
    layers.push_back(&application_settings_);
    for (auto it = buffer.modes().minors().rbegin(); it != buffer.modes().minors().rend(); ++it) {
        append_mode_layers(layers, *it);
    }
    if (buffer.modes().major()) {
        append_mode_layers(layers, *buffer.modes().major());
    }
    return SettingsResolver(setting_definitions_, std::move(layers));
}

SelectionEditPolicy EditorRuntime::selection_edit_policy(ViewId view_id) const {
    const View& view = views_.get(view_id);
    const std::optional<InputStrategyId> strategy =
        view.input_strategy() ? view.input_strategy() : input_strategies_.default_strategy();
    return strategy ? input_strategies_.definition(*strategy).selection_after_edit
                    : SelectionEditPolicy::Collapse;
}

std::optional<LanguageProviderId> EditorRuntime::language_provider(BufferId buffer_id,
                                                                   LanguageFacet facet) const {
    const Buffer& buffer = buffers_.get(buffer_id);
    const std::optional<ModeId> major = buffer.modes().major();
    if (!major) {
        return std::nullopt;
    }
    return language_provider(*major, facet);
}

std::optional<LanguageProviderId> EditorRuntime::language_provider(ModeId mode,
                                                                   LanguageFacet facet) const {
    const std::optional<LanguageProfileId> language = modes_.definition(mode).language;
    return language ? languages_.profile(*language).provider(facet) : std::nullopt;
}

void EditorRuntime::set_default_input_strategy(std::optional<InputStrategyId> strategy) {
    input_strategies_.set_default(strategy);
    views_.refresh_mode_input_states();
}

EditorRuntime::ExtensionCheckpoint EditorRuntime::checkpoint_extensions() const {
    return ExtensionCheckpoint(*this);
}

void EditorRuntime::restore_extensions(const ExtensionCheckpoint& checkpoint) {
    commands_ = checkpoint.commands;
    keymaps_ = checkpoint.keymaps;
    input_states_ = checkpoint.input_states;
    input_strategies_ = checkpoint.input_strategies;
    things_ = checkpoint.things;
    motions_ = checkpoint.motions;
    languages_ = checkpoint.languages;
    modes_ = checkpoint.modes;
    resource_policies_ = checkpoint.resource_policies;
    interaction_providers_ = checkpoint.interaction_providers;
    extensions_sealed_ = checkpoint.extensions_sealed;
}

} // namespace cind
