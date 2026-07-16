#pragma once

#include "editor/command.hpp"
#include "editor/ids.hpp"
#include "editor/mode.hpp"

namespace cind {

class EditorRuntime;

struct LocationListCommands {
    CommandId visit;
    CommandId next;
    CommandId previous;
};

struct LocationListModeRegistration {
    ModeId mode;
    KeymapId keymap;
};

LocationListModeRegistration ensure_location_list_mode(EditorRuntime& runtime,
                                                       LocationListCommands commands);

} // namespace cind
