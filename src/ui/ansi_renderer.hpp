#pragma once

#include "presentation/theme.hpp"
#include "ui/scene.hpp"

#include <string>

namespace cind::ui {

// The terminal presenter: renders a Scene into one ANSI escape-sequence
// string (full-frame repaint; the caller writes it in a single flush).
// Pure — testable against golden strings without a pty. A frame-diffing
// variant for remote transports can sit next to this one later.
std::string render_ansi(const Scene& scene, const PresentationTheme& theme);

} // namespace cind::ui
