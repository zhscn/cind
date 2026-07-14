#pragma once

#include "document/text.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace cind {

// Byte access over a contiguous buffer — the trivial CharSource. The lexer
// and parser template over this and TextCharSource.
struct StringCharSource {
    std::string_view text;

    std::size_t size() const { return text.size(); }
    char at(std::size_t pos) const { return text[pos]; }
    char operator[](std::size_t pos) const { return text[pos]; }
    bool starts_with(std::size_t pos, std::string_view s) const {
        return text.substr(pos).starts_with(s);
    }
    void extract(std::size_t pos, std::size_t len, std::string& out) const {
        out.assign(text.substr(pos, len));
    }
};

// Byte access over a chunked Text value. Caches the chunk holding the last
// access; sequential and line-local scans stay on the cache, and leaving it
// costs one O(log n) re-seek. The Text must outlive the source.
class TextCharSource {
public:
    explicit TextCharSource(const Text& text) : text_(text), size_(text.size_bytes()) {}

    std::size_t size() const { return size_; }
    char at(std::size_t pos) const {
        if (pos < window_start_ || pos >= window_end_) {
            refill(pos);
        }
        return window_[pos - window_start_];
    }
    char operator[](std::size_t pos) const { return at(pos); }
    bool starts_with(std::size_t pos, std::string_view s) const {
        if (pos + s.size() > size_) {
            return false;
        }
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (at(pos + i) != s[i]) {
                return false;
            }
        }
        return true;
    }
    void extract(std::size_t pos, std::size_t len, std::string& out) const {
        out.clear();
        for (std::size_t i = 0; i < len; ++i) {
            out.push_back(at(pos + i));
        }
    }

private:
    void refill(std::size_t pos) const {
        TextCursor cursor(text_, TextOffset{static_cast<std::uint32_t>(pos)});
        window_ = cursor.chunk();
        window_start_ = pos;
        window_end_ = pos + window_.size();
    }

    const Text& text_;
    std::size_t size_;
    mutable std::string_view window_;
    mutable std::size_t window_start_ = 0;
    mutable std::size_t window_end_ = 0;
};

} // namespace cind
