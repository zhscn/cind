#include "ui/view_tree.hpp"

#include <format>

namespace cind::ui {

namespace {

constexpr std::size_t layer_index(ViewLayer layer) {
    return static_cast<std::size_t>(layer);
}

std::string_view role_name(RegionRole role) {
    switch (role) {
    case RegionRole::TextArea:
        return "text-area";
    case RegionRole::LineNumbers:
        return "line-numbers";
    case RegionRole::ChangeSigns:
        return "change-signs";
    case RegionRole::StatusBar:
        return "status-bar";
    case RegionRole::EchoArea:
        return "echo-area";
    case RegionRole::Popup:
        return "popup";
    case RegionRole::Documentation:
        return "documentation";
    }
    return "unknown";
}

} // namespace

ViewLayer view_layer(VerticalAnchor anchor) {
    switch (anchor) {
    case VerticalAnchor::Grid:
    case VerticalAnchor::PaneGrid:
        return ViewLayer::Grid;
    case VerticalAnchor::Cell:
    case VerticalAnchor::Bottom:
        return ViewLayer::Chrome;
    case VerticalAnchor::Overlay:
        return ViewLayer::Overlay;
    }
    return ViewLayer::Grid;
}

std::string region_view_id(const Region& region) {
    if (!region.id.empty()) {
        return region.id;
    }
    return std::format("region:{}", role_name(region.role));
}

std::string_view hit_target_kind_name(HitTargetKind kind) {
    switch (kind) {
    case HitTargetKind::DocumentText:
        return "document-text";
    case HitTargetKind::DocumentGutter:
        return "document-gutter";
    case HitTargetKind::PopupHeader:
        return "popup-header";
    case HitTargetKind::PopupItem:
        return "popup-item";
    case HitTargetKind::Status:
        return "status";
    case HitTargetKind::Echo:
        return "echo";
    case HitTargetKind::Region:
        return "region";
    }
    return "unknown";
}

ViewTree::ViewTree(const Scene& scene)
    : layers_{{{.id = "scene/grid", .layer = ViewLayer::Grid, .children = {}},
               {.id = "scene/chrome", .layer = ViewLayer::Chrome, .children = {}},
               {.id = "scene/overlay", .layer = ViewLayer::Overlay, .children = {}}}} {
    for (std::size_t index = 0; index < scene.regions.size(); ++index) {
        const Region& region = scene.regions[index];
        layers_[layer_index(view_layer(region.vertical_anchor))].children.push_back(
            {.id = region_view_id(region),
             .region_index = index,
             .role = region.role,
             .rect = region.rect});
    }
}

const ViewLayerNode& ViewTree::layer(ViewLayer layer) const {
    return layers_[layer_index(layer)];
}

std::optional<HitTarget> resolve_hit_target(const Scene& scene, const ViewHit& hit) {
    if (hit.region_index >= scene.regions.size()) {
        return std::nullopt;
    }
    const Region& region = scene.regions[hit.region_index];
    HitTarget target{
        .kind = HitTargetKind::Region,
        .view_id = region_view_id(region),
        .pane_id = region.pane_id,
        .region_index = hit.region_index,
        .role = region.role,
        .scene_cell = hit.scene_cell,
        .local_cell = hit.local_cell,
        .document_line = std::nullopt,
        .display_column = std::nullopt,
        .popup_item = std::nullopt,
    };
    if (const Region::DocumentMapping* mapping = region.document_mapping();
        mapping != nullptr && hit.local_cell) {
        target.document_line =
            mapping->first_line + static_cast<std::uint32_t>(hit.local_cell->row);
        if (mapping->first_display_column) {
            target.kind = HitTargetKind::DocumentText;
            target.display_column = *mapping->first_display_column + hit.local_cell->column;
        } else {
            target.kind = HitTargetKind::DocumentGutter;
        }
        return target;
    }
    if (const Region::PopupContent* popup = region.popup()) {
        if (hit.content_index && *hit.content_index > 0 &&
            *hit.content_index <= popup->items.size()) {
            target.kind = HitTargetKind::PopupItem;
            target.popup_item = popup->first_item + *hit.content_index - 1;
        } else {
            target.kind = HitTargetKind::PopupHeader;
        }
        return target;
    }
    if (region.status() != nullptr) {
        target.kind = HitTargetKind::Status;
    } else if (region.echo() != nullptr) {
        target.kind = HitTargetKind::Echo;
    }
    return target;
}

} // namespace cind::ui
