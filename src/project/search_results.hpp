#pragma once

#include "editor/buffer.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

struct LocationListDocument {
    std::string text;
    std::vector<BufferLocation> locations;
};

struct RgSearchResultInput {
    std::string_view project_root;
    std::string_view output;
};

// Parses ripgrep's --null --line-number --column --no-heading stream. The NUL
// path separator keeps filenames containing ':' unambiguous.
std::expected<LocationListDocument, std::string> parse_rg_search_results(RgSearchResultInput input);

} // namespace cind
