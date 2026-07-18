#pragma once

#include "presentation/style.hpp"
#include "presentation/theme.hpp"

namespace cind {

struct PresentationProfile {
    PresentationTheme theme;
    PresentationStyleSheet styles;
    PresentationMotion motion;
    PresentationMetrics metrics;

    friend bool operator==(const PresentationProfile&, const PresentationProfile&) = default;
};

} // namespace cind
