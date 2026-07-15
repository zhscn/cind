#pragma once

namespace cind::gui {

struct SpringState {
    float position = 0.0F;
    float velocity = 0.0F;
};

struct SpringStep {
    float target = 0.0F;
    float angular_frequency = 1.0F;
    float elapsed_seconds = 0.0F;
};

struct SpringRestCriteria {
    float target = 0.0F;
    float position_tolerance = 0.0F;
    float velocity_tolerance = 0.0F;
};

// Advances an analytically integrated critically damped spring. Retargeting
// preserves both fields in SpringState, so position and velocity remain
// continuous across input events.
SpringState advance_critical_spring(SpringState state, SpringStep step);

bool spring_at_rest(SpringState state, SpringRestCriteria criteria);

} // namespace cind::gui
