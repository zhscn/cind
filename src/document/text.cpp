#include "document/text.hpp"

#include <algorithm>
#include <cassert>
#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

namespace detail {
struct TextNode {
    TextSummary summary;
    std::uint32_t height = 0;                               // 0 = leaf
    std::string bytes;                                      // leaves only
    std::vector<std::shared_ptr<const TextNode>> children;  // internal only
};
} // namespace detail

namespace {

using detail::TextNode;
using NodePtr = std::shared_ptr<const TextNode>;

// Tuning: midpoints of the ranges in buffer.md §3; to be settled with the
// bench corpus (buffer.md §8), not guessed.
constexpr std::size_t kMaxChunkBytes = 2048;
constexpr std::size_t kMinChunkBytes = kMaxChunkBytes / 2;
constexpr std::size_t kMaxFanout = 16;
constexpr std::size_t kMinFanout = kMaxFanout / 2;

NodePtr make_leaf(std::string bytes) {
    auto node = std::make_shared<TextNode>();
    node->summary = TextSummary::of(bytes);
    node->bytes = std::move(bytes);
    return node;
}

NodePtr make_internal(std::vector<NodePtr> children) {
    auto node = std::make_shared<TextNode>();
    node->height = children.front()->height + 1;
    for (const NodePtr& child : children) {
        node->summary += child->summary;
    }
    node->children = std::move(children);
    return node;
}

// Greedy cut into pieces sized [kMin, kMax]: take a full piece while at
// least kMin remains afterwards, otherwise split the tail evenly. The same
// scheme groups nodes into parents below.
std::vector<NodePtr> build_leaves(std::string_view text) {
    std::vector<NodePtr> leaves;
    while (text.size() > kMaxChunkBytes) {
        std::size_t take = text.size() >= kMaxChunkBytes + kMinChunkBytes ? kMaxChunkBytes
                                                                          : text.size() / 2;
        leaves.push_back(make_leaf(std::string(text.substr(0, take))));
        text.remove_prefix(take);
    }
    leaves.push_back(make_leaf(std::string(text)));
    return leaves;
}

NodePtr build_tree(std::string_view text) {
    std::vector<NodePtr> level = build_leaves(text);
    while (level.size() > 1) {
        std::vector<NodePtr> parents;
        std::size_t i = 0;
        while (level.size() - i > kMaxFanout) {
            std::size_t take = level.size() - i >= kMaxFanout + kMinFanout ? kMaxFanout
                                                                           : (level.size() - i) / 2;
            parents.push_back(make_internal({level.begin() + static_cast<std::ptrdiff_t>(i),
                                             level.begin() + static_cast<std::ptrdiff_t>(i + take)}));
            i += take;
        }
        parents.push_back(make_internal({level.begin() + static_cast<std::ptrdiff_t>(i), level.end()}));
        level = std::move(parents);
    }
    return level.front();
}

struct NodePair {
    NodePtr first;
    NodePtr second; // null unless the concatenation overflowed one node
};

NodePair pack_children(std::vector<NodePtr> children) {
    if (children.size() <= kMaxFanout) {
        return {make_internal(std::move(children)), nullptr};
    }
    std::size_t mid = children.size() / 2;
    auto split = children.begin() + static_cast<std::ptrdiff_t>(mid);
    return {make_internal({children.begin(), split}), make_internal({split, children.end()})};
}

// Concatenates two subtrees into one or two nodes of height max(a, b).
// Under-filled nodes can only enter (and leave) through the outermost call:
// every node passed down the recursion is a child of a well-formed tree, so
// at each equal-height meeting at least one side is properly filled and the
// merge/redistribute below restores the minimum bounds.
NodePair concat_nodes(const NodePtr& a, const NodePtr& b) {
    if (a->height == b->height) {
        if (a->height == 0) {
            std::size_t total = a->bytes.size() + b->bytes.size();
            if (total > kMaxChunkBytes && a->bytes.size() >= kMinChunkBytes &&
                b->bytes.size() >= kMinChunkBytes) {
                return {a, b};
            }
            std::string all = a->bytes + b->bytes;
            if (all.size() <= kMaxChunkBytes) {
                return {make_leaf(std::move(all)), nullptr};
            }
            std::size_t mid = all.size() / 2;
            return {make_leaf(all.substr(0, mid)), make_leaf(all.substr(mid))};
        }
        std::size_t total = a->children.size() + b->children.size();
        if (total > kMaxFanout && a->children.size() >= kMinFanout &&
            b->children.size() >= kMinFanout) {
            return {a, b};
        }
        std::vector<NodePtr> all = a->children;
        all.insert(all.end(), b->children.begin(), b->children.end());
        return pack_children(std::move(all));
    }
    if (a->height > b->height) {
        NodePair sub = concat_nodes(a->children.back(), b);
        std::vector<NodePtr> merged(a->children.begin(), a->children.end() - 1);
        merged.push_back(std::move(sub.first));
        if (sub.second) {
            merged.push_back(std::move(sub.second));
        }
        return pack_children(std::move(merged));
    }
    NodePair sub = concat_nodes(a, b->children.front());
    std::vector<NodePtr> merged;
    merged.reserve(b->children.size() + 1);
    merged.push_back(std::move(sub.first));
    if (sub.second) {
        merged.push_back(std::move(sub.second));
    }
    merged.insert(merged.end(), b->children.begin() + 1, b->children.end());
    return pack_children(std::move(merged));
}

NodePtr concat_trees(const NodePtr& a, const NodePtr& b) {
    if (!a || a->summary.bytes == 0) {
        return b;
    }
    if (!b || b->summary.bytes == 0) {
        return a;
    }
    NodePair pair = concat_nodes(a, b);
    if (!pair.second) {
        return pair.first;
    }
    return make_internal({std::move(pair.first), std::move(pair.second)});
}

// Subtree for the non-empty range [start, end) within n. Fully covered
// subtrees are shared, not copied. The result's root may be under-filled;
// all deeper nodes keep the invariants.
NodePtr slice_node(const NodePtr& n, std::uint32_t start, std::uint32_t end) {
    if (start == 0 && end == n->summary.bytes) {
        return n;
    }
    if (n->height == 0) {
        return make_leaf(std::string(std::string_view(n->bytes).substr(start, end - start)));
    }
    NodePtr acc;
    std::uint32_t base = 0;
    for (const NodePtr& child : n->children) {
        std::uint32_t child_end = base + child->summary.bytes;
        if (child_end > start && base < end) {
            NodePtr piece = slice_node(child, std::max(start, base) - base,
                                       std::min(end, child_end) - base);
            acc = acc ? concat_trees(acc, piece) : piece;
        }
        base = child_end;
        if (base >= end) {
            break;
        }
    }
    return acc;
}

struct LeafRef {
    const TextNode* leaf;
    TextSummary before; // summary of [0, leaf start)
};

// Walks to the leaf containing offset; an offset on a chunk boundary
// (including offset == total size) lands at the start of the next chunk,
// or in the last leaf when there is none.
LeafRef find_leaf_by_offset(const TextNode* node, std::uint32_t offset) {
    TextSummary before;
    std::uint32_t rel = offset;
    while (node->height > 0) {
        std::size_t i = 0;
        while (i + 1 < node->children.size() && rel >= node->children[i]->summary.bytes) {
            before += node->children[i]->summary;
            rel -= node->children[i]->summary.bytes;
            ++i;
        }
        node = node->children[i].get();
    }
    return {node, before};
}

std::optional<std::string> validate_node(const TextNode* node, bool is_root) {
    if (node->height == 0) {
        if (!node->children.empty()) {
            return "leaf node has children";
        }
        if (node->bytes.size() > kMaxChunkBytes) {
            return std::format("leaf overflows: {} bytes", node->bytes.size());
        }
        if (!is_root && node->bytes.size() < kMinChunkBytes) {
            return std::format("non-root leaf underfilled: {} bytes", node->bytes.size());
        }
        if (node->summary != TextSummary::of(node->bytes)) {
            return "leaf summary mismatch";
        }
        return std::nullopt;
    }
    if (!node->bytes.empty()) {
        return "internal node has bytes";
    }
    if (node->children.size() > kMaxFanout) {
        return std::format("internal node overflows: {} children", node->children.size());
    }
    std::size_t min_children = is_root ? 2 : kMinFanout;
    if (node->children.size() < min_children) {
        return std::format("internal node underfilled: {} children", node->children.size());
    }
    TextSummary sum;
    for (const NodePtr& child : node->children) {
        if (!child) {
            return "null child";
        }
        if (child->height != node->height - 1) {
            return std::format("height mismatch: parent {} child {}", node->height, child->height);
        }
        sum += child->summary;
        if (auto error = validate_node(child.get(), false)) {
            return error;
        }
    }
    if (sum != node->summary) {
        return "internal summary mismatch";
    }
    return std::nullopt;
}

} // namespace

TextSummary TextSummary::of(std::string_view text) {
    TextSummary summary;
    summary.bytes = static_cast<std::uint32_t>(text.size());
    for (char byte : text) {
        auto c = static_cast<unsigned char>(byte);
        summary.lines += c == '\n';
        summary.utf16_units += (c & 0xC0) != 0x80;
        summary.utf16_units += c >= 0xF0;
    }
    return summary;
}

Text::Text() : root_(make_leaf(std::string())) {}

Text::Text(std::string_view text) : root_(build_tree(text)) {}

Text::Text(std::shared_ptr<const detail::TextNode> root) : root_(std::move(root)) {}

std::uint32_t Text::size_bytes() const { return root_->summary.bytes; }

TextSummary Text::summary() const { return root_->summary; }

std::uint32_t Text::line_count() const { return root_->summary.lines + 1; }

std::uint32_t Text::utf16_size() const { return root_->summary.utf16_units; }

void Text::check_line(std::uint32_t line) const {
    if (line >= line_count()) {
        throw std::out_of_range("Text: line out of range");
    }
}

TextOffset Text::line_start(std::uint32_t line) const {
    check_line(line);
    if (line == 0) {
        return TextOffset{0};
    }
    // Line k starts just past the k-th '\n' (1-based).
    const TextNode* node = root_.get();
    std::uint32_t base = 0;
    std::uint32_t k = line;
    while (node->height > 0) {
        std::size_t i = 0;
        while (k > node->children[i]->summary.lines) {
            k -= node->children[i]->summary.lines;
            base += node->children[i]->summary.bytes;
            ++i;
        }
        node = node->children[i].get();
    }
    std::size_t pos = 0;
    for (std::uint32_t seen = 0; seen < k; ++seen) {
        pos = node->bytes.find('\n', pos) + 1;
    }
    return TextOffset{base + static_cast<std::uint32_t>(pos)};
}

TextOffset Text::line_content_end(std::uint32_t line) const {
    check_line(line);
    if (line + 1 < line_count()) {
        return TextOffset{line_start(line + 1).value - 1}; // before the '\n'
    }
    return end_offset();
}

TextRange Text::line_range(std::uint32_t line) const {
    TextOffset start = line_start(line);
    std::uint32_t end = line + 1 < line_count() ? line_start(line + 1).value : size_bytes();
    return make_range(start.value, end);
}

TextRange Text::line_content_range(std::uint32_t line) const {
    return TextRange{line_start(line), line_content_end(line)};
}

LinePosition Text::position(TextOffset offset) const {
    if (offset.value > size_bytes()) {
        throw std::out_of_range("Text: offset out of range");
    }
    auto [leaf, before] = find_leaf_by_offset(root_.get(), offset.value);
    std::string_view prefix = std::string_view(leaf->bytes).substr(0, offset.value - before.bytes);
    auto line = before.lines +
                static_cast<std::uint32_t>(std::count(prefix.begin(), prefix.end(), '\n'));
    std::size_t newline = prefix.rfind('\n');
    std::uint32_t start = newline != std::string_view::npos
                              ? before.bytes + static_cast<std::uint32_t>(newline) + 1
                              : line_start(line).value;
    return LinePosition{line, offset.value - start};
}

TextOffset Text::offset(LinePosition position) const {
    TextRange range = line_range(position.line);
    if (position.byte_column > range.length()) {
        throw std::out_of_range("Text: column out of range");
    }
    return TextOffset{range.start.value + position.byte_column};
}

std::uint32_t Text::utf16_offset(TextOffset offset) const {
    if (offset.value > size_bytes()) {
        throw std::out_of_range("Text: offset out of range");
    }
    auto [leaf, before] = find_leaf_by_offset(root_.get(), offset.value);
    std::string_view prefix = std::string_view(leaf->bytes).substr(0, offset.value - before.bytes);
    return before.utf16_units + TextSummary::of(prefix).utf16_units;
}

TextOffset Text::offset_at_utf16(std::uint32_t utf16) const {
    if (utf16 > utf16_size()) {
        throw std::out_of_range("Text: utf16 offset out of range");
    }
    const TextNode* node = root_.get();
    std::uint32_t base = 0;
    std::uint32_t u = utf16;
    while (node->height > 0) {
        std::size_t i = 0;
        while (i + 1 < node->children.size() && u >= node->children[i]->summary.utf16_units) {
            u -= node->children[i]->summary.utf16_units;
            base += node->children[i]->summary.bytes;
            ++i;
        }
        node = node->children[i].get();
    }
    std::string_view bytes = node->bytes;
    std::uint32_t acc = 0;
    std::size_t pos = 0;
    for (; pos < bytes.size(); ++pos) {
        auto c = static_cast<unsigned char>(bytes[pos]);
        bool lead = (c & 0xC0) != 0x80;
        if (lead && acc >= u) {
            break;
        }
        acc += lead;
        acc += c >= 0xF0;
    }
    std::uint32_t result = base + static_cast<std::uint32_t>(pos);
    // A code point split across chunks can leave the scan on a continuation
    // byte at the chunk edge; round up to the next code point boundary.
    while (result < size_bytes() && (static_cast<unsigned char>(byte_at(TextOffset{result})) & 0xC0) == 0x80) {
        ++result;
    }
    return TextOffset{result};
}

char Text::byte_at(TextOffset offset) const {
    if (offset.value >= size_bytes()) {
        throw std::out_of_range("Text: offset out of range");
    }
    auto [leaf, before] = find_leaf_by_offset(root_.get(), offset.value);
    return leaf->bytes[offset.value - before.bytes];
}

std::string Text::to_string() const { return substring(TextRange{TextOffset{0}, end_offset()}); }

std::string Text::substring(TextRange range) const {
    if (range.start > range.end || range.end.value > size_bytes()) {
        throw std::out_of_range("Text: range out of range");
    }
    std::string out;
    out.reserve(range.length());
    TextCursor cursor(*this, range.start);
    std::uint32_t remaining = range.length();
    while (remaining > 0) {
        std::string_view chunk = cursor.chunk();
        std::size_t take = std::min<std::size_t>(remaining, chunk.size());
        out.append(chunk.substr(0, take));
        remaining -= static_cast<std::uint32_t>(take);
        cursor.advance_chunk();
    }
    return out;
}

Text Text::replace(TextRange range, std::string_view replacement) const {
    std::uint32_t size = size_bytes();
    if (range.start > range.end || range.end.value > size) {
        throw std::out_of_range("Text: range out of range");
    }
    NodePtr prefix = range.start.value > 0 ? slice_node(root_, 0, range.start.value) : nullptr;
    NodePtr middle = replacement.empty() ? nullptr : build_tree(replacement);
    NodePtr suffix = range.end.value < size ? slice_node(root_, range.end.value, size) : nullptr;
    NodePtr result = concat_trees(concat_trees(prefix, middle), suffix);
    if (!result) {
        result = make_leaf(std::string());
    }
    return Text(std::move(result));
}

Text Text::insert(TextOffset position, std::string_view text) const {
    return replace(TextRange{position, position}, text);
}

Text Text::erase(TextRange range) const { return replace(range, std::string_view()); }

std::optional<std::string> Text::validate() const {
    if (!root_) {
        return "null root";
    }
    return validate_node(root_.get(), true);
}

std::optional<TextEdit> diff_edit(const Text& a, const Text& b) {
    const std::uint32_t a_size = a.size_bytes();
    const std::uint32_t b_size = b.size_bytes();
    const std::uint32_t limit = std::min(a_size, b_size);

    // Common prefix: shared chunks (same storage, same cut) skip whole; a
    // divergence falls back to byte comparison inside the chunk pair.
    std::uint32_t prefix = 0;
    while (prefix < limit) {
        TextCursor ca(a, TextOffset{prefix});
        TextCursor cb(b, TextOffset{prefix});
        const std::string_view x = ca.chunk();
        const std::string_view y = cb.chunk();
        if (x.data() == y.data() && x.size() == y.size()) {
            prefix += static_cast<std::uint32_t>(x.size());
            continue;
        }
        const std::size_t n = std::min({x.size(), y.size(),
                                        static_cast<std::size_t>(limit - prefix)});
        std::size_t i = 0;
        while (i < n && x[i] == y[i]) {
            ++i;
        }
        prefix += static_cast<std::uint32_t>(i);
        if (i < n) {
            break;
        }
    }
    if (a_size == b_size && prefix == a_size) {
        return std::nullopt;
    }

    // Common suffix over the bytes past the prefix, walking chunks backwards.
    const std::uint32_t max_suffix = limit - prefix;
    std::uint32_t suffix = 0;
    while (suffix < max_suffix) {
        const std::uint32_t ea = a_size - suffix; // exclusive ends
        const std::uint32_t eb = b_size - suffix;
        TextCursor ca(a, TextOffset{ea - 1});
        TextCursor cb(b, TextOffset{eb - 1});
        const std::string_view x = ca.whole_chunk();
        const std::string_view y = cb.whole_chunk();
        const std::uint32_t wa = ea - ca.whole_chunk_offset().value; // bytes before ea
        const std::uint32_t wb = eb - cb.whole_chunk_offset().value;
        const std::uint32_t step = std::min({wa, wb, max_suffix - suffix});
        if (x.data() == y.data() && wa == wb) {
            suffix += step; // identical chunk, identical cut: bytes must match
            continue;
        }
        std::uint32_t j = 0;
        while (j < step && x[wa - 1 - j] == y[wb - 1 - j]) {
            ++j;
        }
        suffix += j;
        if (j < step) {
            break;
        }
    }

    return TextEdit{make_range(prefix, a_size - suffix),
                    b.substring(make_range(prefix, b_size - suffix))};
}

bool operator==(const Text& a, std::string_view b) {
    if (a.size_bytes() != b.size()) {
        return false;
    }
    std::size_t pos = 0;
    for (TextCursor cursor(a); !cursor.at_end(); cursor.advance_chunk()) {
        const std::string_view chunk = cursor.chunk();
        if (b.substr(pos, chunk.size()) != chunk) {
            return false;
        }
        pos += chunk.size();
    }
    return true;
}

TextCursor::TextCursor(Text text, TextOffset start) : text_(std::move(text)) {
    std::uint32_t size = text_.size_bytes();
    if (start.value > size) {
        throw std::out_of_range("TextCursor: offset out of range");
    }
    if (start.value == size) {
        return; // at end
    }
    const TextNode* node = text_.root_.get();
    std::uint32_t rel = start.value;
    std::uint32_t base = 0;
    while (node->height > 0) {
        std::uint32_t i = 0;
        while (rel >= node->children[i]->summary.bytes) {
            rel -= node->children[i]->summary.bytes;
            base += node->children[i]->summary.bytes;
            ++i;
        }
        stack_.push_back({node, i});
        node = node->children[i].get();
    }
    leaf_ = node;
    leaf_start_ = base;
    skip_ = rel;
}

std::string_view TextCursor::chunk() const {
    if (!leaf_) {
        return {};
    }
    return std::string_view(leaf_->bytes).substr(skip_);
}

std::string_view TextCursor::whole_chunk() const {
    if (!leaf_) {
        return {};
    }
    return leaf_->bytes;
}

TextOffset TextCursor::position() const {
    if (!leaf_) {
        return text_.end_offset();
    }
    return TextOffset{leaf_start_ + skip_};
}

void TextCursor::advance_chunk() {
    if (!leaf_) {
        return;
    }
    leaf_start_ += static_cast<std::uint32_t>(leaf_->bytes.size());
    skip_ = 0;
    while (!stack_.empty()) {
        Frame& frame = stack_.back();
        if (frame.child + 1 < frame.node->children.size()) {
            ++frame.child;
            const TextNode* node = frame.node->children[frame.child].get();
            while (node->height > 0) {
                stack_.push_back({node, 0});
                node = node->children[0].get();
            }
            leaf_ = node;
            return;
        }
        stack_.pop_back();
    }
    leaf_ = nullptr;
}

} // namespace cind
