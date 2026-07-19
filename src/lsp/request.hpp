#pragma once

#include "document/text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cind {

std::string path_to_file_uri(std::string_view path);

// Serialized JSON is the feature boundary. LspSession validates and frames it;
// feature adapters own method-specific construction and parsing.
struct LspRequest {
    std::string method;
    std::string params = "null";
};

struct LspResponse {
    std::string result = "null";
};

struct LspResponseError {
    std::optional<std::int64_t> code;
    std::string message;
    std::string data = "null";
};

struct LspNotification {
    std::string method;
    std::string params = "null";
};

struct LspServerRequest {
    std::string method;
    std::string params = "null";
};

struct LspDocumentSnapshot {
    std::string uri;
    std::string language_id;
    RevisionId revision = 0;
    Text text;
};

} // namespace cind
