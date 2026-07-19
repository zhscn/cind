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

std::optional<unsigned char> hex_byte(char high, char low) {
    const auto digit = [](char value) -> std::optional<unsigned char> {
        if (value >= '0' && value <= '9') {
            return static_cast<unsigned char>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<unsigned char>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<unsigned char>(value - 'A' + 10);
        }
        return std::nullopt;
    };
    const std::optional<unsigned char> first = digit(high);
    const std::optional<unsigned char> second = digit(low);
    if (!first || !second) {
        return std::nullopt;
    }
    return static_cast<unsigned char>((*first << 4U) | *second);
}

std::optional<TextOffset> offset_from_lsp(const Text& text, LspPosition position) {
    if (position.line >= text.line_count()) {
        return std::nullopt;
    }
    const TextOffset line_start = text.line_start(position.line);
    const TextOffset line_end = text.line_content_end(position.line);
    const std::uint32_t line_utf16 = text.utf16_offset(line_start);
    const std::uint64_t absolute = static_cast<std::uint64_t>(line_utf16) + position.character;
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

std::expected<std::string, std::string> file_uri_to_path(std::string_view uri) {
    constexpr std::string_view prefix = "file://";
    if (!uri.starts_with(prefix)) {
        return std::unexpected("LSP location URI is not a file URI");
    }
    uri.remove_prefix(prefix.size());
    if (!uri.starts_with('/')) {
        constexpr std::string_view localhost = "localhost";
        if (!uri.starts_with(localhost) || uri.size() == localhost.size() ||
            uri[localhost.size()] != '/') {
            return std::unexpected("remote file URI authorities are unsupported");
        }
        uri.remove_prefix(localhost.size());
    }
    std::string path;
    path.reserve(uri.size());
    for (std::size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] != '%') {
            path.push_back(uri[index]);
            continue;
        }
        if (index + 2 >= uri.size()) {
            return std::unexpected("file URI has an incomplete percent escape");
        }
        const std::optional<unsigned char> byte = hex_byte(uri[index + 1], uri[index + 2]);
        if (!byte || *byte == 0) {
            return std::unexpected("file URI has an invalid percent escape");
        }
        path.push_back(static_cast<char>(*byte));
        index += 2;
    }
    if (path.empty() || !std::filesystem::path(path).is_absolute()) {
        return std::unexpected("file URI does not contain an absolute path");
    }
    return std::filesystem::path(path).lexically_normal().string();
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
