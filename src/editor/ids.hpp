#pragma once

#include <compare>
#include <cstdint>
#include <limits>

namespace cind {

template <typename Tag> struct EntityId {
    static constexpr std::uint32_t invalid_slot = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t slot = invalid_slot;
    std::uint32_t generation = 0;

    constexpr bool valid() const { return slot != invalid_slot && generation != 0; }
    explicit constexpr operator bool() const { return valid(); }

    friend constexpr auto operator<=>(EntityId, EntityId) = default;
};

struct BufferTag;
struct ProjectTag;
struct ViewTag;
struct WorkbenchTag;
struct WindowTag;

using BufferId = EntityId<BufferTag>;
using ProjectId = EntityId<ProjectTag>;
using ViewId = EntityId<ViewTag>;
using WorkbenchId = EntityId<WorkbenchTag>;
using WindowId = EntityId<WindowTag>;

struct KeymapId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(KeymapId, KeymapId) = default;
};

} // namespace cind
