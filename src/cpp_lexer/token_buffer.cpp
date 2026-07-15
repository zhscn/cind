#include "cpp_lexer/token_buffer.hpp"

#include <algorithm>
#include <cassert>

namespace cind {

TokenBuffer::TokenBuffer(const std::vector<Token>& toks) {
    count_ = toks.size();
    chunks_.reserve(count_ / kChunkCap + 1);
    first_.reserve(count_ / kChunkCap + 1);
    for (std::size_t at = 0; at < count_; at += kChunkCap) {
        const std::size_t n = std::min(kChunkCap, count_ - at);
        Chunk c;
        c.base = toks[at].range.start.value;
        c.toks.assign(toks.begin() + static_cast<std::ptrdiff_t>(at),
                      toks.begin() + static_cast<std::ptrdiff_t>(at + n));
        for (Token& t : c.toks) {
            t.range.start.value -= c.base;
            t.range.end.value -= c.base;
        }
        first_.push_back(at);
        chunks_.push_back(std::move(c));
    }
}

std::size_t TokenBuffer::chunk_of(std::size_t i) const {
    assert(i < count_);
    std::size_t c = cursor_;
    if (c >= chunks_.size() || i < first_[c]) {
        // Behind the cursor: step back once (common backward scan), else search.
        if (c > 0 && c - 1 < chunks_.size() && i >= first_[c - 1]) {
            c = c - 1;
        } else {
            c = static_cast<std::size_t>(
                    std::upper_bound(first_.begin(), first_.end(), i) - first_.begin()) -
                1;
        }
    } else {
        while (c + 1 < chunks_.size() && i >= first_[c + 1]) {
            if (i >= first_[c + 1] + chunks_[c + 1].toks.size()) {
                c = static_cast<std::size_t>(
                        std::upper_bound(first_.begin(), first_.end(), i) - first_.begin()) -
                    1;
                break;
            }
            ++c;
        }
    }
    cursor_ = c;
    return c;
}

void TokenBuffer::push_back(const Token& t) {
    if (chunks_.empty() || chunks_.back().toks.size() >= kChunkCap) {
        Chunk c;
        c.base = t.range.start.value;
        c.toks.reserve(kChunkCap);
        first_.push_back(count_);
        chunks_.push_back(std::move(c));
    }
    Chunk& c = chunks_.back();
    Token rel = t;
    rel.range.start.value -= c.base;
    rel.range.end.value -= c.base;
    c.toks.push_back(rel);
    ++count_;
}

void TokenBuffer::splice(std::size_t lo, std::size_t hi, const std::vector<Token>& replacement,
                         std::int64_t delta) {
    assert(lo <= hi && hi <= count_);

    // Chunk index ranges: prefix chunks fully before lo stay untouched;
    // suffix chunks fully at/after hi survive with base += delta; everything
    // between is repacked together with the replacement.
    std::size_t ck_lo = 0;
    while (ck_lo < chunks_.size() && first_[ck_lo] + chunks_[ck_lo].toks.size() <= lo) {
        ++ck_lo;
    }
    std::size_t ck_hi = ck_lo;
    while (ck_hi < chunks_.size() && first_[ck_hi] < hi) {
        ++ck_hi;
    }
    // Fragments of the boundary chunks that survive the cut.
    std::vector<Token> head;
    if (ck_lo < chunks_.size() && first_[ck_lo] < lo) {
        const Chunk& c = chunks_[ck_lo];
        for (std::size_t k = 0; k < lo - first_[ck_lo]; ++k) {
            head.push_back(materialize(c, k));
        }
    }
    std::vector<Token> tail;
    if (ck_hi > ck_lo && first_[ck_hi - 1] + chunks_[ck_hi - 1].toks.size() > hi) {
        const Chunk& c = chunks_[ck_hi - 1];
        for (std::size_t k = hi - first_[ck_hi - 1]; k < c.toks.size(); ++k) {
            Token t = materialize(c, k);
            t.range.start.value = static_cast<std::uint32_t>(t.range.start.value + delta);
            t.range.end.value = static_cast<std::uint32_t>(t.range.end.value + delta);
            tail.push_back(t);
        }
    }

    std::vector<Chunk> rebuilt;
    rebuilt.reserve(ck_lo + (head.size() + replacement.size() + tail.size()) / kChunkCap + 2 +
                    (chunks_.size() - ck_hi));
    for (std::size_t k = 0; k < ck_lo; ++k) {
        rebuilt.push_back(std::move(chunks_[k]));
    }
    const auto pack = [&](const Token& t) {
        if (rebuilt.size() <= ck_lo || rebuilt.back().toks.size() >= kChunkCap) {
            Chunk c;
            c.base = t.range.start.value;
            c.toks.reserve(kChunkCap);
            rebuilt.push_back(std::move(c));
        }
        Chunk& c = rebuilt.back();
        Token rel = t;
        rel.range.start.value -= c.base;
        rel.range.end.value -= c.base;
        c.toks.push_back(rel);
    };
    for (const Token& t : head) {
        pack(t);
    }
    for (const Token& t : replacement) {
        pack(t);
    }
    for (const Token& t : tail) {
        pack(t);
    }
    for (std::size_t k = ck_hi; k < chunks_.size(); ++k) {
        chunks_[k].base = static_cast<std::uint32_t>(chunks_[k].base + delta);
        rebuilt.push_back(std::move(chunks_[k]));
    }
    chunks_ = std::move(rebuilt);

    count_ = count_ - (hi - lo) + replacement.size();
    first_.resize(chunks_.size());
    std::size_t running = 0;
    for (std::size_t k = 0; k < chunks_.size(); ++k) {
        first_[k] = running;
        running += chunks_[k].toks.size();
    }
    assert(running == count_);
    cursor_ = 0;
}

std::vector<Token> TokenBuffer::flatten() const {
    std::vector<Token> out;
    out.reserve(count_);
    for (const Chunk& c : chunks_) {
        for (std::size_t k = 0; k < c.toks.size(); ++k) {
            out.push_back(materialize(c, k));
        }
    }
    return out;
}

bool TokenBuffer::validate() const {
    if (chunks_.size() != first_.size()) {
        return false;
    }
    std::size_t running = 0;
    for (std::size_t k = 0; k < chunks_.size(); ++k) {
        if (chunks_[k].toks.empty() || first_[k] != running) {
            return false;
        }
        running += chunks_[k].toks.size();
    }
    return running == count_;
}

} // namespace cind
