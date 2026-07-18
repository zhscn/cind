#pragma once

#include "editor/buffer.hpp"
#include "editor/ids.hpp"
#include "formatting/cpp_indent_style.hpp"

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
    CppIndentStyle style;
    std::string style_origin;
    std::optional<std::string> resource_to_open;
    bool startup_placeholder = false;
};

struct SessionFacts {
    bool has_initial_text = false;
};

struct SessionPlan {
    StartupBufferPlan buffer;
};

} // namespace cind
