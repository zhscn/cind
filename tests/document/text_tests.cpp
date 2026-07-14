#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "document/line_index.hpp"
#include "document/text.hpp"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace cind;

namespace {

std::uint32_t ref_utf16(std::string_view text) {
    std::uint32_t units = 0;
    for (char byte : text) {
        auto c = static_cast<unsigned char>(byte);
        units += (c & 0xC0) != 0x80;
        units += c >= 0xF0;
    }
    return units;
}

bool is_code_point_start(std::string_view text, std::size_t offset) {
    return offset >= text.size() ||
           (static_cast<unsigned char>(text[offset]) & 0xC0) != 0x80;
}

std::string via_chunks(const Text& text, TextOffset start = TextOffset{0}) {
    std::string out;
    TextOffset expected = start;
    for (TextCursor cursor(text, start); !cursor.at_end(); cursor.advance_chunk()) {
        REQUIRE(cursor.position() == expected);
        REQUIRE(!cursor.chunk().empty());
        out.append(cursor.chunk());
        expected.value += static_cast<std::uint32_t>(cursor.chunk().size());
    }
    return out;
}

// Full comparison against the reference model: bytes, line queries (LineIndex
// as oracle), UTF-16 conversions on sampled code-point boundaries.
void check_against_model(const Text& text, const std::string& model, std::mt19937& rng) {
    auto error = text.validate();
    INFO(error.value_or(""));
    REQUIRE(!error.has_value());
    REQUIRE(text.size_bytes() == model.size());
    REQUIRE(text.to_string() == model);

    LineIndex index(model);
    REQUIRE(text.line_count() == index.line_count());
    REQUIRE(text.utf16_size() == ref_utf16(model));

    std::uniform_int_distribution<std::uint32_t> offset_dist(
        0, static_cast<std::uint32_t>(model.size()));
    for (int i = 0; i < 24; ++i) {
        TextOffset offset{offset_dist(rng)};
        LinePosition pos = text.position(offset);
        CHECK(pos == index.position(offset));
        CHECK(text.offset(pos) == offset);

        std::uint32_t line = pos.line;
        CHECK(text.line_start(line) == index.line_start(line));
        CHECK(text.line_content_end(line) == index.line_content_end(line));
        CHECK(text.line_range(line) == index.line_range(line));

        std::uint32_t aligned = offset.value;
        while (!is_code_point_start(model, aligned)) {
            ++aligned;
        }
        std::uint32_t units = ref_utf16(std::string_view(model).substr(0, aligned));
        CHECK(text.utf16_offset(TextOffset{aligned}) == units);
        CHECK(text.offset_at_utf16(units) == TextOffset{aligned});

        if (offset.value < model.size()) {
            CHECK(text.byte_at(offset) == model[offset.value]);
        }
    }
}

std::string random_piece(std::mt19937& rng, std::size_t max_len) {
    static const std::vector<std::string> atoms = {
        "a", "z", " ", "\n", "\n\n", "é", "€", "𝄞", "int x = 0;\n", "// comment\n",
    };
    std::uniform_int_distribution<std::size_t> len_dist(0, max_len);
    std::uniform_int_distribution<std::size_t> atom_dist(0, atoms.size() - 1);
    std::string out;
    std::size_t target = len_dist(rng);
    while (out.size() < target) {
        out += atoms[atom_dist(rng)];
    }
    return out;
}

} // namespace

TEST_CASE("text: empty value") {
    Text text;
    REQUIRE(!text.validate().has_value());
    CHECK(text.empty());
    CHECK(text.size_bytes() == 0);
    CHECK(text.line_count() == 1);
    CHECK(text.utf16_size() == 0);
    CHECK(text.to_string().empty());
    CHECK(text.position(TextOffset{0}) == LinePosition{0, 0});
    CHECK(text.line_range(0) == make_range(0, 0));
    CHECK(TextCursor(text).at_end());
    CHECK_THROWS_AS(text.byte_at(TextOffset{0}), std::out_of_range);
    CHECK_THROWS_AS(text.position(TextOffset{1}), std::out_of_range);
    CHECK_THROWS_AS(text.line_start(1), std::out_of_range);
}

TEST_CASE("text: small single-chunk queries are exhaustive-correct") {
    std::string model = "int main() {\n    return 0;\n}\n";
    Text text(model);
    REQUIRE(!text.validate().has_value());
    REQUIRE(text.to_string() == model);
    LineIndex index(model);
    REQUIRE(text.line_count() == index.line_count());
    for (std::uint32_t line = 0; line < index.line_count(); ++line) {
        CHECK(text.line_start(line) == index.line_start(line));
        CHECK(text.line_content_end(line) == index.line_content_end(line));
        CHECK(text.line_range(line) == index.line_range(line));
        CHECK(text.line_content_range(line) == index.line_content_range(line));
    }
    for (std::uint32_t off = 0; off <= model.size(); ++off) {
        LinePosition pos = text.position(TextOffset{off});
        CHECK(pos == index.position(TextOffset{off}));
        CHECK(text.offset(pos) == TextOffset{off});
        CHECK(text.utf16_offset(TextOffset{off}) == off); // pure ASCII
        CHECK(text.offset_at_utf16(off) == TextOffset{off});
    }
}

TEST_CASE("text: utf16 conversions on multibyte content") {
    // a(1B/1u) é(2B/1u) €(3B/1u) 𝄞(4B/2u) \n(1B/1u)
    Text text(std::string_view("aé€𝄞\n"));
    REQUIRE(text.size_bytes() == 11);
    REQUIRE(text.utf16_size() == 6);
    REQUIRE(text.line_count() == 2);

    const std::vector<std::pair<std::uint32_t, std::uint32_t>> boundaries = {
        {0, 0}, {1, 1}, {3, 2}, {6, 3}, {10, 5}, {11, 6},
    };
    for (auto [byte, unit] : boundaries) {
        CHECK(text.utf16_offset(TextOffset{byte}) == unit);
        CHECK(text.offset_at_utf16(unit) == TextOffset{byte});
    }
    // Inside the surrogate pair of 𝄞: rounds up to the next code point.
    CHECK(text.offset_at_utf16(4) == TextOffset{10});
    CHECK_THROWS_AS(text.offset_at_utf16(7), std::out_of_range);
}

TEST_CASE("text: multi-chunk construction and edits") {
    std::mt19937 rng(7);
    std::string model;
    for (int i = 0; i < 3000; ++i) {
        model += "line ";
        model += std::to_string(i);
        model += i % 37 == 0 ? " é𝄞\n" : "\n";
    }
    Text text(model);
    check_against_model(text, model, rng);

    // Cross-chunk replace, erase, insert.
    auto mid = static_cast<std::uint32_t>(model.size() / 2);
    text = text.replace(make_range(mid - 3000, mid + 3000), "REPLACED\n");
    model.replace(mid - 3000, 6000, "REPLACED\n");
    check_against_model(text, model, rng);

    text = text.erase(make_range(0, 5000));
    model.erase(0, 5000);
    check_against_model(text, model, rng);

    text = text.insert(text.end_offset(), "tail");
    model += "tail";
    check_against_model(text, model, rng);

    CHECK_THROWS_AS(text.replace(make_range(0, text.size_bytes() + 1), ""), std::out_of_range);
}

TEST_CASE("text: values are persistent — edits do not disturb held copies") {
    std::string base(100000, 'x');
    for (std::size_t i = 0; i < base.size(); i += 61) {
        base[i] = '\n';
    }
    Text original(base);
    Text edited = original;
    std::string model = base;
    for (std::size_t i = 0; i < 200; ++i) {
        auto offset = static_cast<std::uint32_t>((i * 499) % (model.size() - 10));
        edited = edited.replace(make_range(offset, offset + 3), "ABCDE");
        model.replace(offset, 3, "ABCDE");
    }
    CHECK(original.to_string() == base);
    CHECK(original.line_count() == LineIndex(base).line_count());
    CHECK(edited.to_string() == model);
    REQUIRE(!original.validate().has_value());
    REQUIRE(!edited.validate().has_value());
}

TEST_CASE("text: chunk cursor covers the text from any start") {
    std::string model;
    for (int i = 0; i < 2000; ++i) {
        model += "chunk cursor content é𝄞\n";
    }
    Text text(model);
    CHECK(via_chunks(text) == model);
    for (std::uint32_t start : {1u, 2047u, 2048u, 2049u,
                                static_cast<std::uint32_t>(model.size()) - 1,
                                static_cast<std::uint32_t>(model.size())}) {
        CHECK(via_chunks(text, TextOffset{start}) == model.substr(start));
    }
    CHECK(text.substring(make_range(100, 9000)) == model.substr(100, 8900));
    CHECK_THROWS_AS(TextCursor(text, TextOffset{static_cast<std::uint32_t>(model.size()) + 1}),
                    std::out_of_range);
}

TEST_CASE("text: randomized edits match std::string model") {
    std::mt19937 rng(20260714);
    std::string model;
    Text text;
    std::vector<std::pair<Text, std::string>> held; // snapshot isolation probes

    for (int op = 0; op < 500; ++op) {
        std::uniform_int_distribution<std::uint32_t> offset_dist(
            0, static_cast<std::uint32_t>(model.size()));
        std::uint32_t a = offset_dist(rng);
        std::uint32_t b = offset_dist(rng);
        if (a > b) {
            std::swap(a, b);
        }
        std::uniform_int_distribution<int> kind_dist(0, 9);
        int kind = kind_dist(rng);
        if (kind < 3 || model.empty()) {
            // Insert; occasionally a large block to force deep trees.
            std::string piece = random_piece(rng, kind == 0 ? 10000 : 40);
            text = text.insert(TextOffset{a}, piece);
            model.insert(a, piece);
        } else if (kind < 6 && model.size() < 150000) {
            std::string piece = random_piece(rng, 30);
            text = text.replace(make_range(a, b), piece);
            model.replace(a, b - a, piece);
        } else {
            // Erase; allow long ranges once the model has grown large.
            std::uint32_t end = model.size() >= 150000 ? b : std::min(b, a + 200);
            text = text.erase(make_range(a, end));
            model.erase(a, end - a);
        }
        check_against_model(text, model, rng);

        if (op % 50 == 25) {
            held.emplace_back(text, model);
        }
        if (op % 97 == 0 && !model.empty()) {
            std::uint32_t start = std::min(a, static_cast<std::uint32_t>(model.size() - 1));
            std::uint32_t end = std::min<std::uint32_t>(
                start + 5000, static_cast<std::uint32_t>(model.size()));
            CHECK(text.substring(make_range(start, end)) == model.substr(start, end - start));
            CHECK(via_chunks(text, TextOffset{start}) == model.substr(start));
        }
    }

    for (const auto& [snapshot, expected] : held) {
        CHECK(snapshot.to_string() == expected);
        REQUIRE(!snapshot.validate().has_value());
    }
}
