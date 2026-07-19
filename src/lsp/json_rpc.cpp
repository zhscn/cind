#include "lsp/json_rpc.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <optional>

namespace cind {

std::expected<std::vector<std::string>, std::string> JsonRpcFramer::push(std::string_view bytes) {
    constexpr std::size_t maximum_message = std::size_t{64} * 1024 * 1024;
    buffer_.append(bytes);
    std::vector<std::string> messages;
    while (true) {
        if (!reading_body_) {
            const std::size_t header_end = buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (buffer_.size() > std::size_t{64} * 1024) {
                    reset();
                    return std::unexpected("JSON-RPC header exceeds 64 KiB");
                }
                break;
            }
            std::optional<std::size_t> content_length;
            std::size_t line_start = 0;
            while (line_start < header_end) {
                const std::size_t line_end = buffer_.find("\r\n", line_start);
                const std::size_t end = std::min(line_end, header_end);
                const std::string_view line(buffer_.data() + line_start, end - line_start);
                constexpr std::string_view prefix = "Content-Length:";
                if (line.starts_with(prefix)) {
                    std::string_view value = line.substr(prefix.size());
                    while (!value.empty() && value.front() == ' ') {
                        value.remove_prefix(1);
                    }
                    std::size_t parsed = 0;
                    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                                        parsed);
                    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
                        reset();
                        return std::unexpected("invalid JSON-RPC Content-Length");
                    }
                    content_length = parsed;
                }
                line_start = end + 2;
            }
            if (!content_length || *content_length > maximum_message) {
                reset();
                return std::unexpected(content_length ? "JSON-RPC body exceeds 64 MiB"
                                                      : "JSON-RPC message has no Content-Length");
            }
            body_size_ = *content_length;
            buffer_.erase(0, header_end + 4);
            reading_body_ = true;
        }
        if (buffer_.size() < body_size_) {
            break;
        }
        messages.push_back(buffer_.substr(0, body_size_));
        buffer_.erase(0, body_size_);
        body_size_ = 0;
        reading_body_ = false;
    }
    return messages;
}

void JsonRpcFramer::reset() {
    buffer_.clear();
    body_size_ = 0;
    reading_body_ = false;
}

std::string frame_json_rpc(std::string_view json) {
    return std::format("Content-Length: {}\r\n\r\n{}", json.size(), json);
}

} // namespace cind
