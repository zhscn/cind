#include "gui/scroll_timeline.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace cind::gui {

namespace {

constexpr float scroll_top_tolerance = 0.0001F;

bool same_scroll_top(float left, float right) {
    return std::abs(left - right) <= scroll_top_tolerance;
}

} // namespace

void ScrollSceneTimeline::insert(ui::Scene scene, float scroll_top) {
    insert(std::make_shared<const ui::Scene>(std::move(scene)), scroll_top);
}

void ScrollSceneTimeline::insert(std::shared_ptr<const ui::Scene> scene, float scroll_top) {
    if (!scene) {
        return;
    }
    const auto position = std::ranges::lower_bound(
        keyframes_, scroll_top, {}, [](const Keyframe& keyframe) { return keyframe.scroll_top; });
    if (position != keyframes_.end() && same_scroll_top(position->scroll_top, scroll_top)) {
        if (*position->scene != *scene) {
            position->scene = std::move(scene);
        }
        position->scroll_top = scroll_top;
        return;
    }
    if (position != keyframes_.begin()) {
        const auto previous = std::prev(position);
        if (same_scroll_top(previous->scroll_top, scroll_top)) {
            if (*previous->scene != *scene) {
                previous->scene = std::move(scene);
            }
            previous->scroll_top = scroll_top;
            return;
        }
    }
    keyframes_.insert(position, {.scene = std::move(scene), .scroll_top = scroll_top});
}

void ScrollSceneTimeline::retain_motion_range(float visual_scroll_top, float target_scroll_top) {
    if (keyframes_.size() <= 2) {
        return;
    }
    const float first = std::min(visual_scroll_top, target_scroll_top);
    const float last = std::max(visual_scroll_top, target_scroll_top);
    const auto inside = [first, last](const Keyframe& keyframe) {
        return keyframe.scroll_top + scroll_top_tolerance >= first &&
               keyframe.scroll_top - scroll_top_tolerance <= last;
    };

    std::optional<std::size_t> nearest_before;
    std::optional<std::size_t> nearest_after;
    for (std::size_t index = 0; index < keyframes_.size(); ++index) {
        if (keyframes_[index].scroll_top < first - scroll_top_tolerance) {
            nearest_before = index;
        } else if (keyframes_[index].scroll_top > last + scroll_top_tolerance) {
            nearest_after = index;
            break;
        }
    }

    std::vector<Keyframe> retained;
    retained.reserve(keyframes_.size());
    for (std::size_t index = 0; index < keyframes_.size(); ++index) {
        if (inside(keyframes_[index]) || nearest_before == index || nearest_after == index) {
            retained.push_back(std::move(keyframes_[index]));
        }
    }
    keyframes_ = std::move(retained);
}

std::vector<ScrollSceneLayer> ScrollSceneTimeline::layers_at(float visual_scroll_top) const {
    if (keyframes_.empty()) {
        return {};
    }

    const Keyframe* lower = nullptr;
    const Keyframe* upper = nullptr;
    for (const Keyframe& keyframe : keyframes_) {
        if (keyframe.scroll_top <= visual_scroll_top + scroll_top_tolerance) {
            lower = &keyframe;
        }
        if (keyframe.scroll_top >= visual_scroll_top - scroll_top_tolerance) {
            upper = &keyframe;
            break;
        }
    }
    if (lower == nullptr) {
        lower = &keyframes_.front();
    }
    if (upper == nullptr) {
        upper = &keyframes_.back();
    }

    std::vector<ScrollSceneLayer> result;
    result.push_back({.scene = lower->scene, .scroll_top = lower->scroll_top});
    if (upper != lower) {
        result.push_back({.scene = upper->scene, .scroll_top = upper->scroll_top});
    }
    return result;
}

std::size_t ScrollSceneTimeline::size() const {
    return keyframes_.size();
}

} // namespace cind::gui
