#include "lsp/diagnostics.hpp"
#include "lsp/protocol_json.hpp"

#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

using Json = lsp_json::Json;

LspRange parse_range(const Json& value) {
    const std::optional<LspRange> range = lsp_json::parse_range(value);
    if (!range) {
        throw std::runtime_error("diagnostic range contains an invalid position");
    }
    return *range;
}

DiagnosticSeverity parse_severity(const Json& value) {
    const std::optional<std::int64_t> parsed = lsp_json::int64(value);
    if (!parsed) {
        return DiagnosticSeverity::Error;
    }
    switch (*parsed) {
    case 1:
        return DiagnosticSeverity::Error;
    case 2:
        return DiagnosticSeverity::Warning;
    case 3:
        return DiagnosticSeverity::Information;
    case 4:
        return DiagnosticSeverity::Hint;
    default:
        return DiagnosticSeverity::Error;
    }
}

std::string parse_code(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (const std::optional<std::int64_t> parsed = lsp_json::int64(value)) {
        return std::to_string(*parsed);
    }
    return {};
}

LspPublishedDiagnostics parse_publish(std::string_view payload) {
    const Json params = lsp_json::parse_or_throw(payload, "publishDiagnostics payload");
    if (!params.is_object() || !params.contains("uri") || !params["uri"].is_string() ||
        !params.contains("diagnostics") || !params["diagnostics"].is_array()) {
        throw std::runtime_error("publishDiagnostics has an invalid payload");
    }
    LspPublishedDiagnostics published{
        .uri = params["uri"].get<std::string>(), .version = std::nullopt, .diagnostics = {}};
    if (const Json* version = lsp_json::find(params, "version");
        version != nullptr && !version->is_null()) {
        const std::optional<std::uint64_t> parsed = lsp_json::uint64(*version);
        if (!parsed) {
            throw std::runtime_error("publishDiagnostics version is invalid");
        }
        published.version = static_cast<RevisionId>(*parsed);
    }
    published.diagnostics.reserve(params["diagnostics"].size());
    for (const Json& value : params["diagnostics"].get_array()) {
        if (!value.is_object() || !value.contains("range") || !value.contains("message") ||
            !value["message"].is_string()) {
            throw std::runtime_error("publishDiagnostics contains an invalid diagnostic");
        }
        published.diagnostics.push_back(
            {.range = parse_range(value["range"]),
             .severity = value.contains("severity") ? parse_severity(value["severity"])
                                                    : DiagnosticSeverity::Error,
             .message = value["message"].get<std::string>(),
             .source = value.contains("source") && value["source"].is_string()
                           ? value["source"].get<std::string>()
                           : std::string(),
             .code = value.contains("code") ? parse_code(value["code"]) : std::string()});
    }
    return published;
}

void report_failure(const LspDiagnosticsFeature::Failed& failed, std::string message) noexcept {
    try {
        failed(std::move(message));
    } catch (...) {
        return;
    }
}

} // namespace

void LspDiagnosticsFeature::attach(LspSession& session, Published published, Failed failed) {
    if (!published || !failed) {
        throw std::invalid_argument("LSP diagnostics requires publish and failure callbacks");
    }
    session.set_notification_handler(
        "textDocument/publishDiagnostics",
        [published = std::move(published),
         failed = std::move(failed)](const LspNotification& notification) mutable {
            try {
                published(parse_publish(notification.params));
            } catch (const std::exception& exception) {
                report_failure(failed,
                               std::format("cannot decode LSP diagnostics: {}", exception.what()));
            } catch (...) {
                report_failure(failed, "cannot decode LSP diagnostics");
            }
        });
}

std::string LspDiagnosticsFeature::client_capabilities() {
    return lsp_json::dump(Json{{"textDocument",
                                {{"publishDiagnostics",
                                  {{"relatedInformation", true},
                                   {"versionSupport", true},
                                   {"tagSupport", {{"valueSet", Json::array_t{1U, 2U}}}}}}}}});
}

} // namespace cind
