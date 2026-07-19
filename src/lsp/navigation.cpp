#include "lsp/navigation.hpp"
#include "lsp/protocol_json.hpp"

#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

using Json = lsp_json::Json;

std::optional<LspLocation> parse_location(const Json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    const Json* uri = nullptr;
    const Json* range = nullptr;
    if (const Json* found = lsp_json::find(value, "uri")) {
        uri = found;
        if (const Json* found_range = lsp_json::find(value, "range")) {
            range = found_range;
        }
    } else if (const Json* target_uri = lsp_json::find(value, "targetUri")) {
        uri = target_uri;
        if (const Json* selection = lsp_json::find(value, "targetSelectionRange")) {
            range = selection;
        } else if (const Json* target = lsp_json::find(value, "targetRange")) {
            range = target;
        }
    }
    if (uri == nullptr || range == nullptr || !uri->is_string()) {
        return std::nullopt;
    }
    const std::optional<LspRange> parsed_range = lsp_json::parse_range(*range);
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
        for (const Json& value : result.get_array()) {
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
                {"position", lsp_json::position(lsp_position(request.text, request.caret))}};
    if (kind == LspNavigationKind::References) {
        params["context"] = Json::object_t{{"includeDeclaration", request.include_declaration}};
    }
    Failed parse_failed = failed;
    return session.request(
        {.method = method(kind), .params = lsp_json::dump(params)},
        [completed = std::move(completed),
         failed = std::move(parse_failed)](const LspResponse& response) mutable {
            try {
                completed(parse_locations(lsp_json::parse_or_throw(response.result)));
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
    return lsp_json::dump(Json{{"textDocument",
                                {{"definition", {{"linkSupport", true}}},
                                 {"declaration", {{"linkSupport", true}}},
                                 {"implementation", {{"linkSupport", true}}},
                                 {"references", Json::object_t{}}}}});
}

} // namespace cind
