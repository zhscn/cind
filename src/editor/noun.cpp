#include "editor/noun.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

constexpr std::size_t maximum_pattern_depth = 32;

void require_argument_count(const ThingPattern& pattern, std::size_t expected,
                            std::string_view kind) {
    if (pattern.arguments.size() != expected || !pattern.alternatives.empty()) {
        throw std::invalid_argument(std::format("{} thing requires {} argument{}", kind, expected,
                                                expected == 1 ? "" : "s"));
    }
    for (const std::string& argument : pattern.arguments) {
        if (argument.empty()) {
            throw std::invalid_argument(std::format("{} thing arguments must not be empty", kind));
        }
    }
}

} // namespace

void ThingRegistry::validate_pattern(const ThingPattern& pattern, std::size_t depth) {
    if (depth >= maximum_pattern_depth) {
        throw std::invalid_argument("thing pattern nesting exceeds 32 levels");
    }
    switch (pattern.kind) {
    case ThingPatternKind::Pair:
        require_argument_count(pattern, 2, "pair");
        if (pattern.arguments[0] == pattern.arguments[1]) {
            throw std::invalid_argument("pair thing delimiters must differ");
        }
        return;
    case ThingPatternKind::CstNode:
        require_argument_count(pattern, 1, "cst-node");
        return;
    case ThingPatternKind::CharacterClass:
        require_argument_count(pattern, 1, "char-class");
        if (pattern.arguments[0] != "word" && pattern.arguments[0] != "symbol") {
            throw std::invalid_argument(
                std::format("unknown character class '{}'", pattern.arguments[0]));
        }
        return;
    case ThingPatternKind::Multi:
        if (!pattern.arguments.empty() || pattern.alternatives.empty()) {
            throw std::invalid_argument("multi thing requires at least one nested pattern");
        }
        for (const ThingPattern& alternative : pattern.alternatives) {
            validate_pattern(alternative, depth + 1);
        }
        return;
    }
    throw std::invalid_argument("unknown thing pattern kind");
}

ThingId ThingRegistry::define(std::string name, ThingPattern pattern) {
    if (sealed_) {
        throw std::logic_error("thing registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("thing name must not be empty");
    }
    if (by_name_.contains(name)) {
        throw std::invalid_argument(std::format("thing '{}' is already defined", name));
    }
    validate_pattern(pattern);
    const ThingId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back({.name = std::move(name), .pattern = std::move(pattern)});
    by_name_.emplace(definitions_.back().name, id);
    return id;
}

void ThingRegistry::configure(ThingId id, ThingPattern pattern) {
    if (sealed_) {
        throw std::logic_error("thing registry is sealed");
    }
    validate_pattern(pattern);
    const_cast<Definition&>(std::as_const(*this).definition(id)).pattern = std::move(pattern);
}

const ThingRegistry::Definition& ThingRegistry::definition(ThingId id) const {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown thing id");
    }
    return definitions_[id.value];
}

std::optional<ThingId> ThingRegistry::find(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return found == by_name_.end() ? std::nullopt : std::optional<ThingId>{found->second};
}

MotionId MotionRegistry::define(std::string name, MotionMechanism mechanism) {
    if (sealed_) {
        throw std::logic_error("motion registry is sealed");
    }
    if (name.empty()) {
        throw std::invalid_argument("motion name must not be empty");
    }
    if (by_name_.contains(name)) {
        throw std::invalid_argument(std::format("motion '{}' is already defined", name));
    }
    const MotionId id{static_cast<std::uint32_t>(definitions_.size())};
    definitions_.push_back({.name = std::move(name), .mechanism = mechanism});
    by_name_.emplace(definitions_.back().name, id);
    return id;
}

void MotionRegistry::configure(MotionId id, MotionMechanism mechanism) {
    if (sealed_) {
        throw std::logic_error("motion registry is sealed");
    }
    const_cast<Definition&>(std::as_const(*this).definition(id)).mechanism = mechanism;
}

const MotionRegistry::Definition& MotionRegistry::definition(MotionId id) const {
    if (!id.valid() || id.value >= definitions_.size()) {
        throw std::out_of_range("unknown motion id");
    }
    return definitions_[id.value];
}

std::optional<MotionId> MotionRegistry::find(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return found == by_name_.end() ? std::nullopt : std::optional<MotionId>{found->second};
}

} // namespace cind
