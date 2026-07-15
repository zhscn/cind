#pragma once

#include "cpp_lexer/token.hpp"

#include <cstdint>
#include <iterator>
#include <vector>

namespace cind {

// Chunked token storage with chunk-relative byte offsets (design.md §214: the
// token half of the length-encoded tree). Tokens inside a chunk store ranges
// relative to the chunk's base; materialization adds the base back, so the
// visible API still hands out absolute-offset Tokens by value. An edit's
// suffix shift becomes "add delta to every suffix chunk base" — O(chunks),
// not O(tokens) — and a mid-stream splice repacks only the damaged window
// plus the chunk table instead of memmoving the whole flat vector.
//
// Access is single-threaded; operator[] keeps a mutable cursor so the
// parser's mostly-sequential reads stay O(1).
class TokenBuffer {
    struct Chunk {
        std::uint32_t base = 0;   // byte offset added to every token range
        std::vector<Token> toks;  // ranges relative to base
    };

public:
    TokenBuffer() = default;
    // Bulk conversion from a flat lexed stream (chunk-wise copy + rebase).
    explicit TokenBuffer(const std::vector<Token>& toks);

    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    Token operator[](std::size_t i) const {
        const std::size_t c = chunk_of(i);
        return materialize(chunks_[c], i - first_[c]);
    }
    Token front() const { return (*this)[0]; }
    Token back() const { return (*this)[count_ - 1]; }

    // Append (full-lex build path); amortized O(1).
    void push_back(const Token& t);

    // Replaces tokens [lo, hi) with `replacement` (absolute ranges) and adds
    // `delta` to the byte offsets of every surviving token from hi on. The
    // damaged window (boundary chunk fragments + replacement) is repacked
    // into full chunks; untouched prefix chunks are kept as-is and untouched
    // suffix chunks are kept with only their base adjusted. O(chunk count +
    // window size).
    void splice(std::size_t lo, std::size_t hi, const std::vector<Token>& replacement,
                std::int64_t delta);

    // Proxy iterator: dereferences to a Token by value (C++20 random-access
    // via iterator_concept; honest input_iterator_tag for legacy traits).
    class iterator {
    public:
        using value_type = Token;
        using reference = Token;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;

        iterator() = default;
        iterator(const TokenBuffer* buf, std::size_t i) : buf_(buf), i_(i) {}

        // Arrow proxy: `it->kind` works on the materialized value.
        struct arrow_proxy {
            Token t;
            const Token* operator->() const { return &t; }
        };

        Token operator*() const { return (*buf_)[i_]; }
        arrow_proxy operator->() const { return {(*buf_)[i_]}; }
        Token operator[](difference_type n) const {
            return (*buf_)[static_cast<std::size_t>(static_cast<difference_type>(i_) + n)];
        }
        iterator& operator++() { ++i_; return *this; }
        iterator operator++(int) { iterator t = *this; ++i_; return t; }
        iterator& operator--() { --i_; return *this; }
        iterator operator--(int) { iterator t = *this; --i_; return t; }
        iterator& operator+=(difference_type n) {
            i_ = static_cast<std::size_t>(static_cast<difference_type>(i_) + n);
            return *this;
        }
        iterator& operator-=(difference_type n) {
            i_ = static_cast<std::size_t>(static_cast<difference_type>(i_) - n);
            return *this;
        }
        friend iterator operator+(iterator it, difference_type n) { return it += n; }
        friend iterator operator+(difference_type n, iterator it) { return it += n; }
        friend iterator operator-(iterator it, difference_type n) { return it -= n; }
        friend difference_type operator-(const iterator& a, const iterator& b) {
            return static_cast<difference_type>(a.i_) - static_cast<difference_type>(b.i_);
        }
        friend bool operator==(const iterator& a, const iterator& b) { return a.i_ == b.i_; }
        friend auto operator<=>(const iterator& a, const iterator& b) { return a.i_ <=> b.i_; }

    private:
        const TokenBuffer* buf_ = nullptr;
        std::size_t i_ = 0;
    };

    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, count_}; }

    // Materializes the whole stream as a flat absolute-offset vector
    // (chunk-wise; used by the full-reparse fallback so the O(n) parse reads
    // raw memory instead of paying per-token chunk dispatch).
    std::vector<Token> flatten() const;

    // Structural invariants (tests): chunk counts sum to size(), first_ is
    // the prefix-count table, no empty chunks (except an empty buffer).
    bool validate() const;

private:
    static constexpr std::size_t kChunkCap = 512;

    static Token materialize(const Chunk& c, std::size_t k) {
        Token t = c.toks[k];
        t.range.start.value += c.base;
        t.range.end.value += c.base;
        return t;
    }

    std::size_t chunk_of(std::size_t i) const;

    std::vector<Chunk> chunks_;
    std::vector<std::size_t> first_; // first_[k] = global index of chunks_[k][0]
    std::size_t count_ = 0;
    mutable std::size_t cursor_ = 0; // last chunk hit; sequential reads are O(1)
};

} // namespace cind
