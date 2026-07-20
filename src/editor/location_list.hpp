#pragma once

#include "document/anchor.hpp"
#include "document/text_types.hpp"
#include "editor/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace cind {

using LocationListId = std::uint64_t;
inline constexpr LocationListId kInvalidLocationList = 0;

struct LocationRange {
    EncodedLinePosition start;
    EncodedLinePosition end;

    friend bool operator==(const LocationRange&, const LocationRange&) = default;
};

struct ResolvedLocation {
    BufferId buffer;
    AnchorId start = 0;
    AnchorId end = 0;
    bool stale = false;
};

struct LocationItem {
    std::string resource;
    LocationRange range;
    std::string excerpt;
    std::unordered_map<std::string, std::string> metadata;
    std::optional<ResolvedLocation> resolved;
};

struct LocationList {
    LocationListId id = kInvalidLocationList;
    std::string source;
    std::vector<LocationItem> items;
    std::optional<BufferId> materialized_buffer;
    std::uint64_t created_at = 0;
    std::uint64_t version = 1;
};

class LocationListStack {
public:
    LocationListId publish(std::string source, std::vector<LocationItem> items,
                           std::optional<BufferId> materialized_buffer = std::nullopt);
    LocationList* find(LocationListId id);
    const LocationList* find(LocationListId id) const;
    LocationList* find_by_buffer(BufferId buffer);
    const LocationList* find_by_buffer(BufferId buffer) const;
    std::span<const LocationList> lists() const { return lists_; }

    using Resolver = std::function<ResolvedLocation(const LocationItem&)>;
    void resolve_resource(std::string_view resource, const Resolver& resolver);
    void detach_buffer(BufferId buffer,
                       const std::function<LinePosition(AnchorId)>& resolve_position,
                       const std::function<void(AnchorId)>& remove_anchor);
    void release_anchors(const std::function<void(BufferId, AnchorId)>& remove_anchor);

private:
    std::vector<LocationList> lists_;
    LocationListId next_id_ = 0;
    std::uint64_t clock_ = 0;
};

} // namespace cind
