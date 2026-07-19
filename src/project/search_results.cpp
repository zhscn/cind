#include "project/search_results.hpp"

#include <charconv>
#include <filesystem>
#include <format>
#include <limits>

namespace cind {

namespace {

enum class NumberField : std::uint8_t {
    Line,
    Column,
};

std::expected<std::uint32_t, std::string> parse_positive_number(std::string_view value,
                                                                NumberField field) {
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc() || end != value.data() + value.size() || result == 0) {
        return std::unexpected(std::format("invalid ripgrep {} number",
                                           field == NumberField::Line ? "line" : "column"));
    }
    return result;
}

} // namespace

std::expected<LocationListDocument, std::string>
parse_rg_search_results(RgSearchResultInput input) {
    namespace fs = std::filesystem;
    LocationListDocument result;
    const std::string_view output = input.output;
    std::size_t offset = 0;
    while (offset < output.size()) {
        const std::size_t path_end = output.find('\0', offset);
        if (path_end == std::string_view::npos) {
            return std::unexpected("ripgrep result is missing its path separator");
        }
        const std::size_t record_end = output.find('\n', path_end + 1);
        if (record_end == std::string_view::npos) {
            return std::unexpected("ripgrep result is missing its record terminator");
        }
        const std::string_view path_text = output.substr(offset, path_end - offset);
        const std::string_view record = output.substr(path_end + 1, record_end - path_end - 1);
        const std::size_t line_end = record.find(':');
        const std::size_t column_end = line_end == std::string_view::npos
                                           ? std::string_view::npos
                                           : record.find(':', line_end + 1);
        if (path_text.empty() || line_end == std::string_view::npos ||
            column_end == std::string_view::npos) {
            return std::unexpected("ripgrep result has an invalid record header");
        }
        const auto line = parse_positive_number(record.substr(0, line_end), NumberField::Line);
        if (!line) {
            return std::unexpected(line.error());
        }
        const auto column = parse_positive_number(
            record.substr(line_end + 1, column_end - line_end - 1), NumberField::Column);
        if (!column) {
            return std::unexpected(column.error());
        }
        const std::string_view content = record.substr(column_end + 1);

        fs::path resource(path_text);
        if (resource.is_relative()) {
            resource = fs::path(input.project_root) / resource;
        }
        resource = resource.lexically_normal();
        fs::path display(path_text);
        if (!display.empty() && *display.begin() == ".") {
            display = display.lexically_relative(".");
        }
        const std::size_t source_start = result.text.size();
        result.text += std::format("{}:{}:{}: {}\n", display.string(), *line, *column, content);
        if (result.text.size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected("ripgrep result buffer exceeds the document size limit");
        }
        result.locations.push_back(
            {.source_range = make_range(static_cast<std::uint32_t>(source_start),
                                        static_cast<std::uint32_t>(result.text.size())),
             .resource = resource.string(),
             .target = {.line = *line - 1,
                        .column = *column - 1,
                        .encoding = PositionEncoding::Bytes},
             .excerpt = std::string(content)});
        offset = record_end + 1;
    }
    return result;
}

} // namespace cind
