#include "lsp/navigation.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

using Json = nlohmann::json;

Json position_json(LspPosition position) {
    return {{"line", position.line}, {"character", position.character}};
}

std::optional<LspPosition> parse_position(const Json& value) {
    if (!value.is_object() || !value.contains("line") || !value.contains("character") ||
        !value["line"].is_number_unsigned() || !value["character"].is_number_unsigned()) {
        return std::nullopt;
    }
    const auto line = value["line"].get<std::uint64_t>();
    const auto character = value["character"].get<std::uint64_t>();
    if (line > std::numeric_limits<std::uint32_t>::max() ||
        character > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return LspPosition{static_cast<std::uint32_t>(line), static_cast<std::uint32_t>(character)};
}

std::optional<LspRange> parse_range(const Json& value) {
    if (!value.is_object() || !value.contains("start") || !value.contains("end")) {
        return std::nullopt;
    }
    const std::optional<LspPosition> start = parse_position(value["start"]);
    const std::optional<LspPosition> end = parse_position(value["end"]);
    return start && end ? std::optional(LspRange{*start, *end}) : std::nullopt;
}

std::optional<LspLocation> parse_location(const Json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    const Json* uri = nullptr;
    const Json* range = nullptr;
    if (const auto found = value.find("uri"); found != value.end()) {
        uri = &*found;
        const auto found_range = value.find("range");
        if (found_range != value.end()) {
            range = &*found_range;
        }
    } else if (const auto target_uri = value.find("targetUri"); target_uri != value.end()) {
        uri = &*target_uri;
        const auto selection = value.find("targetSelectionRange");
        const auto target = value.find("targetRange");
        if (selection != value.end()) {
            range = &*selection;
        } else if (target != value.end()) {
            range = &*target;
        }
    }
    if (uri == nullptr || range == nullptr || !uri->is_string()) {
        return std::nullopt;
    }
    const std::optional<LspRange> parsed_range = parse_range(*range);
    const std::expected<std::string, std::string> resource =
        file_uri_to_path(uri->get<std::string>());
    if (!parsed_range || !resource) {
        return std::nullopt;
    }
    return LspLocation{.resource = *resource, .range = *parsed_range};
}

std::vector<LspLocation> parse_locations(const Json& result) {
    if (result.is_null()) {
        return {};
    }
    std::vector<LspLocation> locations;
    const auto append = [&](const Json& value) {
        if (std::optional<LspLocation> location = parse_location(value)) {
            locations.push_back(std::move(*location));
        }
    };
    if (result.is_array()) {
        locations.reserve(result.size());
        for (const Json& value : result) {
            append(value);
        }
    } else if (result.is_object()) {
        append(result);
    } else {
        throw std::runtime_error("LSP navigation result is not a location or location array");
    }
    return locations;
}

std::string method(LspNavigationKind kind) {
    return std::format("textDocument/{}", lsp_navigation_name(kind));
}

std::string capability(LspNavigationKind kind) {
    return std::format("{}Provider", lsp_navigation_name(kind));
}

void report_failure(const LspNavigationFeature::Failed& failed, std::string message) noexcept {
    try {
        failed(std::move(message));
    } catch (...) {
        return;
    }
}

} // namespace

std::string_view lsp_navigation_name(LspNavigationKind kind) {
    switch (kind) {
    case LspNavigationKind::Definition:
        return "definition";
    case LspNavigationKind::Declaration:
        return "declaration";
    case LspNavigationKind::Implementation:
        return "implementation";
    case LspNavigationKind::References:
        return "references";
    }
    throw std::logic_error("unknown LSP navigation kind");
}

std::expected<LspNavigationFeature::Cancel, std::string>
LspNavigationFeature::request(LspSession& session, LspNavigationKind kind,
                              LspNavigationRequest request, Completed completed, Failed failed,
                              Cancelled cancelled) {
    if (!completed || !failed) {
        return std::unexpected("LSP navigation requires completion and failure callbacks");
    }
    if (std::expected<void, std::string> synchronized =
            session.synchronize_document({.uri = request.uri,
                                          .language_id = request.language_id,
                                          .revision = request.revision,
                                          .text = request.text});
        !synchronized) {
        return std::unexpected(std::move(synchronized.error()));
    }
    Json params{{"textDocument", {{"uri", request.uri}}},
                {"position", position_json(lsp_position(request.text, request.caret))}};
    if (kind == LspNavigationKind::References) {
        params["context"] = {{"includeDeclaration", request.include_declaration}};
    }
    Failed parse_failed = failed;
    return session.request(
        {.method = method(kind), .params = params.dump()},
        [completed = std::move(completed),
         failed = std::move(parse_failed)](const LspResponse& response) mutable {
            try {
                completed(parse_locations(Json::parse(response.result)));
            } catch (const std::exception& exception) {
                report_failure(failed, std::format("cannot decode LSP navigation response: {}",
                                                   exception.what()));
            } catch (...) {
                report_failure(failed, "cannot decode LSP navigation response");
            }
        },
        [failed = std::move(failed)](LspResponseError error) mutable {
            report_failure(failed, std::move(error.message));
        },
        std::move(cancelled));
}

bool LspNavigationFeature::supported(const LspSession& session, LspNavigationKind kind) {
    return session.capability_boolean({capability(kind)});
}

std::string LspNavigationFeature::client_capabilities() {
    return Json{{"textDocument",
                 {{"definition", {{"linkSupport", true}}},
                  {"declaration", {{"linkSupport", true}}},
                  {"implementation", {{"linkSupport", true}}},
                  {"references", Json::object()}}}}
        .dump();
}

} // namespace cind
