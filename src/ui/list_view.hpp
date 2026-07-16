#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>

namespace cind::ui {

// Retained scroll state for a finite list. The viewport moves only when the
// selected item crosses an edge, so reversing direction within the visible
// range does not move the list underneath the selection.
class ListViewport {
public:
    void reveal(std::optional<std::size_t> selection, std::size_t item_count,
                std::size_t capacity) {
        if (item_count == 0 || capacity == 0 || !selection || *selection >= item_count) {
            first_item_ = 0;
            return;
        }

        capacity = std::min(capacity, item_count);
        const std::size_t maximum_first = item_count - capacity;
        first_item_ = std::min(first_item_, maximum_first);
        if (*selection < first_item_) {
            first_item_ = *selection;
        } else if (*selection >= first_item_ + capacity) {
            first_item_ = std::min(*selection - capacity + 1, maximum_first);
        }
    }

    [[nodiscard]] std::size_t first_item() const { return first_item_; }

private:
    std::size_t first_item_ = 0;
};

} // namespace cind::ui
