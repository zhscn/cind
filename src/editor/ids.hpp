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

using BufferId = EntityId<BufferTag>;
using ProjectId = EntityId<ProjectTag>;
using ViewId = EntityId<ViewTag>;

} // namespace cind
