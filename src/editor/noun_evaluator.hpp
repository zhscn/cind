#pragma once

#include "editor/noun.hpp"
#include "editor/selection.hpp"

#include <cstdint>
#include <optional>

namespace cind {

class DocumentSnapshot;
class SyntaxTree;

enum class ThingExtent : std::uint8_t {
    Inner,
    Bounds,
};

struct ThingMatch {
    SelectionRange inner;
    SelectionRange bounds;
};

std::optional<ThingMatch> evaluate_thing(const ThingRegistry& registry, ThingId thing,
                                         const DocumentSnapshot& snapshot, const SyntaxTree& tree,
                                         TextOffset position);

ViewSelection evaluate_motion(const MotionRegistry& registry, MotionId motion,
                              const DocumentSnapshot& snapshot, const SyntaxTree& tree,
                              const ViewSelection& selection, std::int64_t count, bool extend);

} // namespace cind
