#pragma once

#include "editor/buffer.hpp"
#include "editor/command.hpp"
#include "editor/input_state.hpp"
#include "editor/input_strategy.hpp"
#include "editor/interaction.hpp"
#include "editor/keymap.hpp"
#include "editor/mode.hpp"
#include "editor/project.hpp"
#include "editor/settings.hpp"
#include "editor/view.hpp"
#include "editor/window.hpp"

namespace cind {

// Owns one editor application's registries. Nothing in this object is a
// process-global singleton; callers pass the runtime or a CommandContext
// explicitly at every extension boundary.
class EditorRuntime {
public:
    EditorRuntime();

    SettingRegistry& setting_definitions() { return setting_definitions_; }
    const SettingRegistry& setting_definitions() const { return setting_definitions_; }
    SettingsLayer& application_settings() { return application_settings_; }
    const SettingsLayer& application_settings() const { return application_settings_; }
    LanguageRegistry& languages() { return languages_; }
    const LanguageRegistry& languages() const { return languages_; }
    ModeRegistry& modes() { return modes_; }
    const ModeRegistry& modes() const { return modes_; }
    BufferRegistry& buffers() { return buffers_; }
    const BufferRegistry& buffers() const { return buffers_; }
    ProjectRegistry& projects() { return projects_; }
    const ProjectRegistry& projects() const { return projects_; }
    ViewRegistry& views() { return views_; }
    const ViewRegistry& views() const { return views_; }
    WindowRegistry& windows() { return windows_; }
    const WindowRegistry& windows() const { return windows_; }
    CommandRegistry& commands() { return commands_; }
    const CommandRegistry& commands() const { return commands_; }
    KeymapRegistry& keymaps() { return keymaps_; }
    const KeymapRegistry& keymaps() const { return keymaps_; }
    InputStateRegistry& input_states() { return input_states_; }
    const InputStateRegistry& input_states() const { return input_states_; }
    InputStrategyRegistry& input_strategies() { return input_strategies_; }
    const InputStrategyRegistry& input_strategies() const { return input_strategies_; }
    InteractionProviderRegistry& interaction_providers() { return interaction_providers_; }
    const InteractionProviderRegistry& interaction_providers() const {
        return interaction_providers_;
    }

    // Freezes extension definitions after startup. Runtime values in explicit
    // application/buffer/view layers remain configurable.
    void seal_extensions();
    bool extensions_sealed() const { return extensions_sealed_; }

    SettingsResolver settings_for(BufferId buffer, ViewId view) const;
    void set_default_input_strategy(std::optional<InputStrategyId> strategy);

private:
    void append_mode_layers(std::vector<const SettingsLayer*>& layers, ModeId mode) const;

    SettingRegistry setting_definitions_;
    SettingsLayer application_settings_;
    LanguageRegistry languages_;
    CommandRegistry commands_;
    KeymapRegistry keymaps_;
    InputStateRegistry input_states_;
    InputStrategyRegistry input_strategies_;
    ModeRegistry modes_;
    BufferRegistry buffers_;
    ProjectRegistry projects_;
    ViewRegistry views_;
    WindowRegistry windows_;
    InteractionProviderRegistry interaction_providers_;
    bool extensions_sealed_ = false;
};

} // namespace cind
