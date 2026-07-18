#pragma once

#include <cstdint>

namespace cind {

// Frontend-independent semantic palette. Colors use straight-alpha ARGB;
// presenters convert them to their native pixel or terminal representation.
struct PresentationTheme {
    std::uint32_t canvas = 0;
    std::uint32_t highlight = 0;
    std::uint32_t band = 0;
    std::uint32_t selection = 0;
    std::uint32_t divider = 0;
    std::uint32_t text = 0;
    std::uint32_t strong = 0;
    std::uint32_t faded = 0;
    std::uint32_t faint = 0;
    std::uint32_t salient = 0;
    std::uint32_t popout = 0;
    std::uint32_t critical = 0;
    std::uint32_t cursor = 0;
    std::uint32_t sign_added = 0;
    std::uint32_t sign_modified = 0;
    std::uint32_t sign_deleted = 0;

    friend bool operator==(const PresentationTheme&, const PresentationTheme&) = default;
};

struct PresentationMotion {
    std::uint32_t view_duration_ms = 0;
    float scroll_spring_frequency = 0.0F;
    float scroll_position_tolerance = 0.0F;
    float scroll_velocity_tolerance = 0.0F;

    friend bool operator==(const PresentationMotion&, const PresentationMotion&) = default;
};

} // namespace cind
