#include "editor/location_list.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cind {

LocationListId LocationListStack::publish(std::string source, std::vector<LocationItem> items,
                                          std::optional<BufferId> materialized_buffer) {
    if (source.empty()) {
        throw std::invalid_argument("location list source must not be empty");
    }
    if (std::ranges::any_of(items,
                            [](const LocationItem& item) { return item.resource.empty(); })) {
        throw std::invalid_argument("location list item has no resource");
    }
    if (next_id_ == std::numeric_limits<LocationListId>::max()) {
        throw std::overflow_error("location list id space is exhausted");
    }
    const LocationListId id = ++next_id_;
    lists_.push_back({.id = id,
                      .source = std::move(source),
                      .items = std::move(items),
                      .materialized_buffer = materialized_buffer,
                      .created_at = ++clock_,
                      .version = 1});
    return id;
}

LocationList* LocationListStack::find(LocationListId id) {
    const auto found =
        std::ranges::find_if(lists_, [id](const LocationList& list) { return list.id == id; });
    return found == lists_.end() ? nullptr : &*found;
}

const LocationList* LocationListStack::find(LocationListId id) const {
    return const_cast<LocationListStack*>(this)->find(id);
}

LocationList* LocationListStack::find_by_buffer(BufferId buffer) {
    const auto found = std::ranges::find_if(
        lists_, [buffer](const LocationList& list) { return list.materialized_buffer == buffer; });
    return found == lists_.end() ? nullptr : &*found;
}

const LocationList* LocationListStack::find_by_buffer(BufferId buffer) const {
    return const_cast<LocationListStack*>(this)->find_by_buffer(buffer);
}

void LocationListStack::resolve_resource(std::string_view resource, const Resolver& resolver) {
    for (LocationList& list : lists_) {
        bool changed = false;
        for (LocationItem& item : list.items) {
            if (item.resource != resource || item.resolved) {
                continue;
            }
            item.resolved = resolver(item);
            changed = true;
        }
        if (changed) {
            ++list.version;
        }
    }
}

void LocationListStack::detach_buffer(BufferId buffer,
                                      const std::function<LinePosition(AnchorId)>& resolve_position,
                                      const std::function<void(AnchorId)>& remove_anchor) {
    for (LocationList& list : lists_) {
        bool changed = false;
        if (list.materialized_buffer == buffer) {
            list.materialized_buffer.reset();
            changed = true;
        }
        for (LocationItem& item : list.items) {
            if (!item.resolved || item.resolved->buffer != buffer) {
                continue;
            }
            item.range = {
                .start = EncodedLinePosition::from_bytes(resolve_position(item.resolved->start)),
                .end = EncodedLinePosition::from_bytes(resolve_position(item.resolved->end))};
            remove_anchor(item.resolved->start);
            if (item.resolved->end != item.resolved->start) {
                remove_anchor(item.resolved->end);
            }
            item.resolved.reset();
            changed = true;
        }
        if (changed) {
            ++list.version;
        }
    }
}

void LocationListStack::release_anchors(
    const std::function<void(BufferId, AnchorId)>& remove_anchor) {
    for (LocationList& list : lists_) {
        for (LocationItem& item : list.items) {
            if (!item.resolved) {
                continue;
            }
            remove_anchor(item.resolved->buffer, item.resolved->start);
            if (item.resolved->end != item.resolved->start) {
                remove_anchor(item.resolved->buffer, item.resolved->end);
            }
            item.resolved.reset();
        }
    }
}

} // namespace cind
