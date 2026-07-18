#pragma once

#include "editor/buffer.hpp"
#include "editor/ids.hpp"

#include <optional>
#include <string>

namespace cind {

struct StartupFacts {
    std::string requested_resource;
    bool has_initial_text = false;
};

struct StartupBufferPlan {
    std::string name;
    BufferKind kind = BufferKind::Scratch;
    std::optional<std::string> resource;
    bool read_only = false;
    ModeId major_mode;
    bool use_initial_text = false;
};

struct StartupPlan {
    StartupBufferPlan buffer;
    std::optional<std::string> resource_to_open;
    bool startup_placeholder = false;
};

} // namespace cind
