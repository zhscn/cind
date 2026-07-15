#include "editor/runtime.hpp"

#include <stdexcept>
#include <vector>

namespace cind {

EditorRuntime::EditorRuntime()
    : application_settings_(setting_definitions_, SettingScope::Application),
      languages_(setting_definitions_), modes_(setting_definitions_, languages_),
      buffers_(setting_definitions_), projects_(buffers_, setting_definitions_),
      views_(buffers_, setting_definitions_) {}

void EditorRuntime::seal_extensions() {
    if (extensions_sealed_) {
        return;
    }
    setting_definitions_.seal();
    languages_.seal();
    modes_.seal();
    commands_.seal();
    keymaps_.seal();
    extensions_sealed_ = true;
}

void EditorRuntime::append_mode_layers(std::vector<const SettingsLayer*>& layers,
                                       ModeId mode) const {
    const ModeRegistry::Definition& definition = modes_.definition(mode);
    layers.push_back(&definition.defaults);
    if (definition.language) {
        layers.push_back(&languages_.profile(*definition.language).defaults);
    }
}

SettingsResolver EditorRuntime::settings_for(BufferId buffer_id, ViewId view_id) const {
    const Buffer& buffer = buffers_.get(buffer_id);
    const View& view = views_.get(view_id);
    if (view.buffer_id() != buffer_id) {
        throw std::invalid_argument("view does not display the requested buffer");
    }

    std::vector<const SettingsLayer*> layers;
    layers.push_back(&view.settings());
    layers.push_back(&buffer.settings());

    if (buffer.project_id()) {
        layers.push_back(&projects_.get(*buffer.project_id()).settings());
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

} // namespace cind
