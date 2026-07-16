#pragma once

#include "editor/keymap.hpp"

#include <cstddef>

namespace cind {

class EditorRuntime;

// Installs the standard editor bindings whose named commands exist in the
// runtime. A frontend contributes platform services, not a separate binding
// table; optional editor capabilities become active when their commands are
// registered.
std::size_t bind_default_editor_keys(EditorRuntime& runtime, KeymapId keymap);

// Installs bindings that are valid independently of the focused text target.
// Focus-specific stacks use this map as their final fallback.
std::size_t bind_default_application_keys(EditorRuntime& runtime, KeymapId keymap);

} // namespace cind
