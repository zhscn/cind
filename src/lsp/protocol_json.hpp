#pragma once

#include "lsp/json.hpp"
#include "lsp/protocol.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace cind::lsp_json {

inline Json position(LspPosition value) {
    return {{"line", value.line}, {"character", value.character}};
}

inline std::optional<LspPosition> parse_position(const Json& value) {
    if (!value.is_object() || !value.contains("line") || !value.contains("character") ||
        !uint64(value["line"]) || !uint64(value["character"])) {
        return std::nullopt;
    }
    const std::uint64_t line = *uint64(value["line"]);
    const std::uint64_t character = *uint64(value["character"]);
    if (line > std::numeric_limits<std::uint32_t>::max() ||
        character > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return LspPosition{.line = static_cast<std::uint32_t>(line),
                       .character = static_cast<std::uint32_t>(character)};
}

inline std::optional<LspRange> parse_range(const Json& value) {
    if (!value.is_object() || !value.contains("start") || !value.contains("end")) {
        return std::nullopt;
    }
    const std::optional<LspPosition> start = parse_position(value["start"]);
    const std::optional<LspPosition> end = parse_position(value["end"]);
    return start && end ? std::optional(LspRange{.start = *start, .end = *end}) : std::nullopt;
}

inline std::optional<LspTextEdit> parse_text_edit(const Json& value) {
    if (!value.is_object() || !value.contains("range") || !value.contains("newText") ||
        !value["newText"].is_string()) {
        return std::nullopt;
    }
    const std::optional<LspRange> range = parse_range(value["range"]);
    return range ? std::optional(LspTextEdit{.range = *range,
                                             .new_text = value["newText"].get<std::string>()})
                 : std::nullopt;
}

} // namespace cind::lsp_json
