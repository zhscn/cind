#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cind {

enum class ModelineGroup : std::uint8_t { Chip, Left, Right };
enum class ModelineTone : std::uint8_t { Strong, Normal, Faded, Faint, Salient, Critical };
enum class ModelineWeight : std::uint8_t { Regular, Strong };

struct ModelineSegment {
    std::string text;
    ModelineGroup group = ModelineGroup::Left;
    ModelineTone tone = ModelineTone::Normal;
    ModelineWeight weight = ModelineWeight::Regular;
    bool debug = false;

    friend bool operator==(const ModelineSegment&, const ModelineSegment&) = default;
};

struct ModelineContent {
    std::vector<ModelineSegment> segments;

    friend bool operator==(const ModelineContent&, const ModelineContent&) = default;
};

struct ModelineFacts {
    std::string buffer_name;
    std::string resource;
    bool dirty = false;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t line_count = 0;
    std::uint64_t revision = 0;
    std::string style_origin;
    std::string input_state;
};

} // namespace cind
