#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cind {

struct ThingId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(ThingId, ThingId) = default;
};

enum class ThingPatternKind : std::uint8_t {
    Pair,
    CstNode,
    CharacterClass,
    Multi,
};

struct ThingPattern {
    ThingPatternKind kind = ThingPatternKind::CharacterClass;
    std::vector<std::string> arguments;
    std::vector<ThingPattern> alternatives;

    friend bool operator==(const ThingPattern&, const ThingPattern&) = default;
};

class ThingRegistry {
public:
    struct Definition {
        std::string name;
        ThingPattern pattern;
    };

    ThingId define(std::string name, ThingPattern pattern);
    void configure(ThingId id, ThingPattern pattern);
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(ThingId id) const;
    std::optional<ThingId> find(std::string_view name) const;

private:
    static void validate_pattern(const ThingPattern& pattern, std::size_t depth = 0);

    std::vector<Definition> definitions_;
    std::unordered_map<std::string, ThingId> by_name_;
    bool sealed_ = false;
};

struct MotionId {
    static constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t value = invalid;

    constexpr bool valid() const { return value != invalid; }
    explicit constexpr operator bool() const { return valid(); }
    friend constexpr auto operator<=>(MotionId, MotionId) = default;
};

enum class MotionMechanism : std::uint8_t {
    ForwardCharacter,
    BackwardCharacter,
    ForwardWord,
    ForwardWordEnd,
    BackwardWord,
    ForwardSymbol,
    BackwardSymbol,
    ForwardExpression,
    BackwardExpression,
    UpList,
};

class MotionRegistry {
public:
    struct Definition {
        std::string name;
        MotionMechanism mechanism = MotionMechanism::ForwardCharacter;
    };

    MotionId define(std::string name, MotionMechanism mechanism);
    void configure(MotionId id, MotionMechanism mechanism);
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }

    const Definition& definition(MotionId id) const;
    std::optional<MotionId> find(std::string_view name) const;

private:
    std::vector<Definition> definitions_;
    std::unordered_map<std::string, MotionId> by_name_;
    bool sealed_ = false;
};

} // namespace cind
