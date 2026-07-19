#pragma once

#include "document/text.hpp"
#include "editor/buffer.hpp"
#include "editor/transaction_group.hpp"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace cind {

struct ComposedExcerptSpec {
    BufferId buffer;
    TextRange context;
    TextRange primary;
};

struct ComposedExcerpt {
    BufferId buffer;
    AnchorId context_start = 0;
    AnchorId context_end = 0;
    AnchorId primary_start = 0;
    AnchorId primary_end = 0;
};

struct ComposedSegment {
    std::size_t excerpt = 0;
    TextRange projection;
    TextRange source;
};

struct ComposedSnapshot {
    Text text;
    std::vector<ComposedSegment> segments;
};

// A composed view borrows anchored ranges from real buffers. Its projection is
// rebuilt on demand, while edits map immediately back to the borrowed buffers.
class ComposedViewModel {
public:
    ComposedViewModel(BufferRegistry& buffers, const std::vector<ComposedExcerptSpec>& excerpts);
    ~ComposedViewModel();

    ComposedViewModel(const ComposedViewModel&) = delete;
    ComposedViewModel& operator=(const ComposedViewModel&) = delete;

    std::span<const ComposedExcerpt> excerpts() const { return excerpts_; }
    std::vector<BufferId> buffers() const;
    ComposedSnapshot snapshot() const;
    std::expected<std::vector<TransactionGroupEntry>, std::string>
    apply_edits(std::span<const TextEdit> edits);

private:
    void release_anchors() noexcept;

    BufferRegistry* buffers_;
    std::vector<ComposedExcerpt> excerpts_;
};

} // namespace cind
