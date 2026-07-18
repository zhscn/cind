#pragma once

#include "editor/language_mechanism.hpp"
#include "editor/noun.hpp"
#include "editor/selection.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace cind {

class DocumentSnapshot;
class SyntaxTree;

using StructuralMotionResolver =
    std::function<std::optional<TextOffset>(StructuralMotion, TextOffset)>;

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
                              const DocumentSnapshot& snapshot, const ViewSelection& selection,
                              std::int64_t count, bool extend,
                              const StructuralMotionResolver& structural_motion = {});

std::optional<ViewSelection> evaluate_node_expansion(const SyntaxTree& tree,
                                                     const ViewSelection& selection);

} // namespace cind
