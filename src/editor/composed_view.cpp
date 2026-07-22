#include "editor/composed_view.hpp"

#include "editor/workspace_edit.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

bool valid_excerpt(const Buffer& buffer, const ComposedExcerptSpec& excerpt) {
    const std::uint32_t size = buffer.snapshot().size_bytes();
    return !excerpt.context.empty() && excerpt.context.end.value <= size &&
           excerpt.context.start <= excerpt.primary.start &&
           excerpt.primary.start <= excerpt.primary.end &&
           excerpt.primary.end <= excerpt.context.end;
}

TextOffset projection_offset(std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("composed projection exceeds the text offset range");
    }
    return TextOffset{static_cast<std::uint32_t>(size)};
}

} // namespace

ComposedViewModel::ComposedViewModel(BufferRegistry& buffers,
                                     const std::vector<ComposedExcerptSpec>& excerpts)
    : buffers_(&buffers) {
    std::vector<ComposedExcerptSpec> merged;
    for (const ComposedExcerptSpec& excerpt : excerpts) {
        Buffer& buffer = buffers.get(excerpt.buffer);
        if (!valid_excerpt(buffer, excerpt)) {
            throw std::invalid_argument("composed excerpt is outside its buffer");
        }
        if (!merged.empty() && merged.back().buffer == excerpt.buffer &&
            excerpt.context.start <= merged.back().context.end) {
            merged.back().context.end = std::max(merged.back().context.end, excerpt.context.end);
            merged.back().primary.start =
                std::min(merged.back().primary.start, excerpt.primary.start);
            merged.back().primary.end = std::max(merged.back().primary.end, excerpt.primary.end);
            continue;
        }
        merged.push_back(excerpt);
    }
    excerpts_.reserve(merged.size());
    try {
        for (const ComposedExcerptSpec& excerpt : merged) {
            Buffer& buffer = buffers.get(excerpt.buffer);
            excerpts_.push_back(
                {.buffer = excerpt.buffer,
                 .label = excerpt.label,
                 .context_start = buffer.create_navigation_anchor(excerpt.context.start,
                                                                  AnchorAffinity::BeforeInsertion),
                 .context_end = buffer.create_navigation_anchor(excerpt.context.end),
                 .primary_start = buffer.create_navigation_anchor(excerpt.primary.start,
                                                                  AnchorAffinity::BeforeInsertion),
                 .primary_end = buffer.create_navigation_anchor(excerpt.primary.end)});
        }
    } catch (...) {
        release_anchors();
        throw;
    }
}

ComposedViewModel::~ComposedViewModel() {
    release_anchors();
}

std::vector<BufferId> ComposedViewModel::buffers() const {
    std::vector<BufferId> result;
    for (const ComposedExcerpt& excerpt : excerpts_) {
        if (std::ranges::find(result, excerpt.buffer) == result.end()) {
            result.push_back(excerpt.buffer);
        }
    }
    return result;
}

ComposedSnapshot ComposedViewModel::snapshot() const {
    std::string projection;
    std::vector<ComposedSegment> segments;
    for (std::size_t index = 0; index < excerpts_.size(); ++index) {
        const ComposedExcerpt& excerpt = excerpts_[index];
        const Buffer& buffer = buffers_->get(excerpt.buffer);
        if (!projection.empty()) {
            projection += '\n';
        }
        projection += std::format("── {} ──\n", excerpt.label);
        const TextRange source{buffer.navigation_anchor_offset(excerpt.context_start),
                               buffer.navigation_anchor_offset(excerpt.context_end)};
        const TextOffset start = projection_offset(projection.size());
        projection += buffer.snapshot().substring(source);
        const TextOffset end = projection_offset(projection.size());
        segments.push_back(
            {.excerpt = index, .projection = {.start = start, .end = end}, .source = source});
    }
    return {.text = Text(std::move(projection)), .segments = std::move(segments)};
}

std::expected<std::vector<TransactionGroupEntry>, std::string>
ComposedViewModel::apply_edits(std::span<const TextEdit> edits) {
    const ComposedSnapshot projected = snapshot();
    std::map<BufferId, std::vector<TextEdit>> by_buffer;
    for (const TextEdit& edit : edits) {
        const auto segment =
            std::ranges::find_if(projected.segments, [&](const ComposedSegment& s) {
                return s.projection.start <= edit.old_range.start &&
                       edit.old_range.end <= s.projection.end;
            });
        if (segment == projected.segments.end()) {
            return std::unexpected("composed edit crosses an excerpt boundary");
        }
        const std::uint32_t relative_start =
            edit.old_range.start.value - segment->projection.start.value;
        const std::uint32_t relative_end =
            edit.old_range.end.value - segment->projection.start.value;
        by_buffer[excerpts_[segment->excerpt].buffer].push_back(
            {.old_range = {.start = TextOffset{segment->source.start.value + relative_start},
                           .end = TextOffset{segment->source.start.value + relative_end}},
             .new_text = edit.new_text});
    }

    std::vector<WorkspaceBufferEdit> workspace_edit;
    workspace_edit.reserve(by_buffer.size());
    for (auto& [buffer_id, buffer_edits] : by_buffer) {
        Buffer& buffer = buffers_->get(buffer_id);
        workspace_edit.push_back({.buffer = buffer_id,
                                  .revision = buffer.snapshot().revision(),
                                  .edits = std::move(buffer_edits)});
    }
    return apply_workspace_edit(*buffers_, workspace_edit);
}

void ComposedViewModel::release_anchors() noexcept {
    for (const ComposedExcerpt& excerpt : excerpts_) {
        try {
            Buffer& buffer = buffers_->get(excerpt.buffer);
            buffer.remove_navigation_anchor(excerpt.context_start);
            buffer.remove_navigation_anchor(excerpt.context_end);
            buffer.remove_navigation_anchor(excerpt.primary_start);
            buffer.remove_navigation_anchor(excerpt.primary_end);
        } catch (...) {
            continue;
        }
    }
    excerpts_.clear();
}

} // namespace cind
