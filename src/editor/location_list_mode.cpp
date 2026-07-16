#include "editor/location_list_mode.hpp"

#include "editor/runtime.hpp"

#include <stdexcept>

namespace cind {

LocationListModeRegistration ensure_location_list_mode(EditorRuntime& runtime,
                                                       LocationListCommands commands) {
    constexpr std::string_view mode_name = "cind.location-list";
    constexpr std::string_view keymap_name = "cind.location-list.map";
    if (const std::optional<ModeId> mode = runtime.modes().find(mode_name)) {
        const std::optional<KeymapId> keymap = runtime.keymaps().find(keymap_name);
        if (!keymap) {
            throw std::logic_error("incomplete built-in location-list mode registration");
        }
        return {.mode = *mode, .keymap = *keymap};
    }
    if (!commands.visit || !commands.next || !commands.previous) {
        throw std::invalid_argument("location-list mode requires all navigation commands");
    }

    const KeymapId keymap = runtime.keymaps().define(std::string(keymap_name));
    runtime.keymaps().bind(keymap, "RET", commands.visit);
    runtime.keymaps().bind(keymap, "M-n", commands.next);
    runtime.keymaps().bind(keymap, "M-p", commands.previous);
    const ModeId mode = runtime.modes().define(std::string(mode_name), ModeKind::Major);
    runtime.modes().definition_for_configuration(mode).keymaps.push_back(keymap);
    return {.mode = mode, .keymap = keymap};
}

} // namespace cind
