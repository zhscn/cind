#include "gui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cind::gui {

SpringState advance_critical_spring(SpringState state, SpringStep step) {
    if (!(step.angular_frequency > 0.0F)) {
        throw std::invalid_argument("spring angular frequency must be positive");
    }
    const float elapsed = std::max(0.0F, step.elapsed_seconds);
    const float displacement = state.position - step.target;
    const float coefficient = state.velocity + step.angular_frequency * displacement;
    const float decay = std::exp(-step.angular_frequency * elapsed);
    return {
        .position = step.target + (displacement + coefficient * elapsed) * decay,
        .velocity = (state.velocity - step.angular_frequency * coefficient * elapsed) * decay,
    };
}

bool spring_at_rest(SpringState state, SpringRestCriteria criteria) {
    return std::abs(state.position - criteria.target) <=
               std::max(0.0F, criteria.position_tolerance) &&
           std::abs(state.velocity) <= std::max(0.0F, criteria.velocity_tolerance);
}

} // namespace cind::gui
