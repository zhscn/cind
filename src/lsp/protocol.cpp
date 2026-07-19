#include "lsp/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

namespace cind {

namespace {

bool uri_unreserved(unsigned char byte) {
    return std::isalnum(byte) != 0 || byte == '-' || byte == '.' || byte == '_' || byte == '~' ||
           byte == '/';
}

std::optional<TextOffset> offset_from_lsp(const Text& text, LspPosition position) {
    if (position.line >= text.line_count()) {
        return std::nullopt;
    }
    const TextOffset line_start = text.line_start(position.line);
    const TextOffset line_end = text.line_content_end(position.line);
    const std::uint32_t line_utf16 = text.utf16_offset(line_start);
    const std::uint64_t absolute =
        static_cast<std::uint64_t>(line_utf16) + position.character;
    if (absolute > text.utf16_size()) {
        return std::nullopt;
    }
    const TextOffset offset = text.offset_at_utf16(static_cast<std::uint32_t>(absolute));
    return offset <= line_end ? std::optional(offset) : std::nullopt;
}

} // namespace

std::string path_to_file_uri(std::string_view path) {
    std::string normalized =
        std::filesystem::absolute(std::filesystem::path(path)).lexically_normal().string();
    std::string uri = "file://";
    uri.reserve(uri.size() + normalized.size());
    for (const char character : normalized) {
        const auto byte = static_cast<unsigned char>(character);
        if (uri_unreserved(byte)) {
            uri.push_back(static_cast<char>(byte));
        } else {
            uri.append(std::format("%{:02X}", byte));
        }
    }
    return uri;
}

LspPosition lsp_position(const Text& text, TextOffset offset) {
    const LinePosition position = text.position(offset);
    const TextOffset line_start = text.line_start(position.line);
    return {.line = position.line,
            .character = text.utf16_offset(offset) - text.utf16_offset(line_start)};
}

std::optional<TextRange> text_range_from_lsp(const Text& text, LspRange range) {
    const std::optional<TextOffset> start = offset_from_lsp(text, range.start);
    const std::optional<TextOffset> end = offset_from_lsp(text, range.end);
    if (!start || !end || *start > *end) {
        return std::nullopt;
    }
    return TextRange{*start, *end};
}

} // namespace cind
