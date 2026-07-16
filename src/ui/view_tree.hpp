#pragma once

#include "ui/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cind::ui {

enum class ViewLayer : std::uint8_t {
    Grid,
    Chrome,
    Overlay,
};

struct ViewNode {
    std::string id;
    std::size_t region_index = 0;
    RegionRole role = RegionRole::TextArea;
    Rect rect;
};

struct ViewLayerNode {
    std::string id;
    ViewLayer layer = ViewLayer::Grid;
    std::vector<ViewNode> children;
};

// Ordered semantic hierarchy derived from a Scene. Presenters paint children
// in layer order and hit-test them in reverse order, so z-order is explicit
// and independent of incidental Region vector traversal.
class ViewTree {
public:
    explicit ViewTree(const Scene& scene);

    const ViewLayerNode& layer(ViewLayer layer) const;
    std::span<const ViewLayerNode> layers() const { return layers_; }

private:
    std::array<ViewLayerNode, 3> layers_;
};

struct ViewHit {
    std::size_t region_index = 0;
    std::optional<CellPoint> scene_cell;
    std::optional<CellPoint> local_cell;
    // Semantic content index: popup header is zero and visible popup rows are
    // one-based. Primitive regions use the primitive index when available.
    std::optional<std::size_t> content_index;
};

enum class HitTargetKind : std::uint8_t {
    DocumentText,
    DocumentGutter,
    PopupHeader,
    PopupItem,
    Status,
    Echo,
    Region,
};

struct HitTarget {
    HitTargetKind kind = HitTargetKind::Region;
    std::string view_id;
    std::string pane_id;
    std::size_t region_index = 0;
    RegionRole role = RegionRole::TextArea;
    std::optional<CellPoint> scene_cell;
    std::optional<CellPoint> local_cell;
    std::optional<std::uint32_t> document_line;
    std::optional<int> display_column;
    std::optional<std::size_t> popup_item;
};

ViewLayer view_layer(VerticalAnchor anchor);
std::string region_view_id(const Region& region);
std::string_view hit_target_kind_name(HitTargetKind kind);
std::optional<HitTarget> resolve_hit_target(const Scene& scene, const ViewHit& hit);

} // namespace cind::ui
