#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cpp_lexer/token_buffer.hpp"

#include <algorithm>
#include <random>
#include <vector>

using namespace cind;

namespace {

Token tok(std::uint32_t start, std::uint32_t len, TokenKind kind = TokenKind::Identifier) {
    return Token{kind, make_range(start, start + len), LexicalFlags::None};
}

// Contiguous stream of n tokens with pseudo-random lengths.
std::vector<Token> stream(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<Token> v;
    std::uint32_t at = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t len = 1 + rng() % 9;
        v.push_back(tok(at, len));
        at += len;
    }
    return v;
}

void expect_equal(const TokenBuffer& buf, const std::vector<Token>& ref) {
    REQUIRE(buf.validate());
    REQUIRE(buf.size() == ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
        CAPTURE(i);
        const Token t = buf[i];
        REQUIRE(t.kind == ref[i].kind);
        REQUIRE(t.range == ref[i].range);
        REQUIRE(t.flags == ref[i].flags);
    }
}

// The reference semantics of splice: shift the suffix, then replace the window.
void ref_splice(std::vector<Token>& v, std::size_t lo, std::size_t hi,
                const std::vector<Token>& replacement, std::int64_t delta) {
    for (std::size_t i = hi; i < v.size(); ++i) {
        v[i].range.start.value = static_cast<std::uint32_t>(v[i].range.start.value + delta);
        v[i].range.end.value = static_cast<std::uint32_t>(v[i].range.end.value + delta);
    }
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(lo), v.begin() + static_cast<std::ptrdiff_t>(hi));
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(lo), replacement.begin(), replacement.end());
}

} // namespace

TEST_CASE("token buffer: push_back and bulk construction agree") {
    const std::vector<Token> ref = stream(1500, 42);
    TokenBuffer bulk(ref);
    TokenBuffer grown;
    for (const Token& t : ref) {
        grown.push_back(t);
    }
    expect_equal(bulk, ref);
    expect_equal(grown, ref);
    expect_equal(TokenBuffer(bulk.flatten()), ref);
}

TEST_CASE("token buffer: random access after sequential and backward reads") {
    const std::vector<Token> ref = stream(3000, 7);
    TokenBuffer buf(ref);
    // forward, backward, then random probes exercise the cursor paths
    for (std::size_t i = 0; i < ref.size(); ++i) {
        REQUIRE(buf[i].range == ref[i].range);
    }
    for (std::size_t i = ref.size(); i-- > 0;) {
        REQUIRE(buf[i].range == ref[i].range);
    }
    std::mt19937 rng(3);
    for (int k = 0; k < 2000; ++k) {
        const std::size_t i = rng() % ref.size();
        REQUIRE(buf[i].range == ref[i].range);
    }
}

TEST_CASE("token buffer: iterator works with ranges algorithms") {
    const std::vector<Token> ref = stream(2000, 9);
    TokenBuffer buf(ref);
    // lower_bound by start offset, as the indent service does
    const std::uint32_t probe = ref[1234].range.start.value;
    auto it = std::ranges::lower_bound(buf, TextOffset{probe}, {},
                                       [](const Token& t) { return t.range.start; });
    REQUIRE(it != buf.end());
    REQUIRE((*it).range.start.value == probe);
    REQUIRE(static_cast<std::size_t>(it - buf.begin()) == 1234);
}

TEST_CASE("token buffer: splice fuzz against the flat reference") {
    std::mt19937 rng(20260715u);
    for (int round = 0; round < 30; ++round) {
        CAPTURE(round);
        std::vector<Token> ref = stream(1 + rng() % 4000, static_cast<unsigned>(rng()));
        TokenBuffer buf(ref);
        for (int step = 0; step < 40; ++step) {
            CAPTURE(step);
            const std::size_t lo = rng() % (ref.size() + 1);
            const std::size_t hi = lo + rng() % (ref.size() - lo + 1);
            // Replacement re-lexes [start of lo, ...) with a new length; the
            // suffix shifts by the length delta, keeping the stream contiguous.
            const std::uint32_t start =
                lo < ref.size() ? ref[lo].range.start.value
                                : (ref.empty() ? 0 : ref.back().range.end.value);
            const std::uint32_t old_end =
                hi < ref.size() ? ref[hi].range.start.value
                                : (ref.empty() ? 0 : ref.back().range.end.value);
            std::vector<Token> repl;
            std::uint32_t at = start;
            const std::size_t repl_n = rng() % 40;
            for (std::size_t i = 0; i < repl_n; ++i) {
                const std::uint32_t len = 1 + rng() % 9;
                repl.push_back(tok(at, len));
                at += len;
            }
            const std::int64_t delta =
                static_cast<std::int64_t>(at) - static_cast<std::int64_t>(old_end);
            buf.splice(lo, hi, repl, delta);
            ref_splice(ref, lo, hi, repl, delta);
            expect_equal(buf, ref);
        }
    }
}
