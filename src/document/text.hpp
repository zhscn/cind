#pragma once

#include "document/text_types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

namespace detail {
struct TextNode;
}

// Additive per-subtree aggregate (a monoid: default value is the identity,
// operator+ is associative). utf16_units counts one UTF-16 code unit per
// code point plus one extra per 4-byte sequence; contributions are attached
// to lead bytes, so sums stay correct across arbitrary chunk splits.
struct TextSummary {
    std::uint32_t bytes = 0;
    std::uint32_t lines = 0;       // number of '\n' bytes
    std::uint32_t utf16_units = 0; // UTF-16 code units of the UTF-8 content

    static TextSummary of(std::string_view text);

    constexpr TextSummary& operator+=(TextSummary other) {
        bytes += other.bytes;
        lines += other.lines;
        utf16_units += other.utf16_units;
        return *this;
    }
    friend constexpr TextSummary operator+(TextSummary a, TextSummary b) { return a += b; }
    friend constexpr bool operator==(TextSummary, TextSummary) = default;
};

// Immutable UTF-8 text value backed by a persistent chunked B+-tree.
// Copying is O(1) (shares the whole tree); replace/insert/erase return new
// values that share every untouched subtree with the original, so holding
// arbitrarily many revisions costs O(edited bytes). Line and UTF-16 lookups
// ride the per-node summaries in O(log n).
//
// Line semantics match LineIndex: lines split on '\n' only, a trailing '\n'
// yields a final empty line, and there is always >= 1 line.
class Text {
public:
    Text();
    explicit Text(std::string_view text);

    std::uint32_t size_bytes() const;
    bool empty() const { return size_bytes() == 0; }
    TextOffset end_offset() const { return TextOffset{size_bytes()}; }
    TextSummary summary() const;
    std::uint32_t line_count() const;
    std::uint32_t utf16_size() const;

    TextOffset line_start(std::uint32_t line) const;
    // Offset just past the last content byte, before the terminating '\n'.
    TextOffset line_content_end(std::uint32_t line) const;
    // [line_start, next line_start): includes the terminating '\n' if present.
    TextRange line_range(std::uint32_t line) const;
    TextRange line_content_range(std::uint32_t line) const;
    // offset must be <= size_bytes(); size_bytes() maps to the last line.
    LinePosition position(TextOffset offset) const;
    TextOffset offset(LinePosition position) const;

    // UTF-16 code unit count of [0, offset). offset should lie on a code
    // point boundary.
    std::uint32_t utf16_offset(TextOffset offset) const;
    // Smallest code-point-aligned offset o with utf16_offset(o) >= utf16;
    // a position inside a surrogate pair rounds up to the next code point.
    TextOffset offset_at_utf16(std::uint32_t utf16) const;

    char byte_at(TextOffset offset) const;
    std::string to_string() const;
    std::string substring(TextRange range) const;

    Text replace(TextRange range, std::string_view replacement) const;
    Text insert(TextOffset position, std::string_view text) const;
    Text erase(TextRange range) const;

    // Test/debug aid: recomputes every summary and checks node heights,
    // fanout and chunk-size bounds. Returns a description of the first
    // violation, or nullopt if the tree is well-formed.
    std::optional<std::string> validate() const;

    // Content comparison, chunk-wise, without materializing.
    friend bool operator==(const Text& a, std::string_view b);

    // Structural diff: the smallest single edit turning a into b, with the
    // shared prefix and suffix skipped chunk-wise (pointer-equal chunks
    // compare in O(1), so revisions that share structure diff in
    // O(changed bytes + log n)). Returns nullopt when the contents are
    // equal. The result is in a's coordinates.
    friend std::optional<TextEdit> diff_edit(const Text& a, const Text& b);
    // Same scan, but returns the changed windows of both sides instead of
    // materializing the replacement — for per-frame consumers (change signs)
    // where copying the window would defeat the O(changed + log n) bound.
    friend std::optional<DiffSpans> diff_spans(const Text& a, const Text& b);

private:
    friend class TextCursor;

    explicit Text(std::shared_ptr<const detail::TextNode> root);
    void check_line(std::uint32_t line) const;

    std::shared_ptr<const detail::TextNode> root_; // never null
};

// Forward-only chunk cursor over a Text value. Holds a reference-counted
// copy of the text, so it stays valid regardless of the source's lifetime.
class TextCursor {
public:
    explicit TextCursor(Text text, TextOffset start = TextOffset{});

    // Contiguous bytes from the current position to the end of the current
    // chunk; empty exactly when the cursor is at the end of the text.
    std::string_view chunk() const;
    // Offset of chunk().front(); size_bytes() when at the end.
    TextOffset position() const;
    // The whole chunk containing the current position (chunk() without the
    // leading skip) and the text offset of its first byte.
    std::string_view whole_chunk() const;
    TextOffset whole_chunk_offset() const { return TextOffset{leaf_start_}; }
    bool at_end() const { return leaf_ == nullptr; }
    void advance_chunk();

private:
    struct Frame {
        const detail::TextNode* node;
        std::uint32_t child;
    };

    Text text_;
    std::vector<Frame> stack_;               // path from root to the current leaf
    const detail::TextNode* leaf_ = nullptr; // null at end
    std::uint32_t leaf_start_ = 0;           // text offset of the current leaf's first byte
    std::uint32_t skip_ = 0;                 // bytes skipped at the front of the current leaf
};

} // namespace cind
