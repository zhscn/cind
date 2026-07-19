#include "lsp/diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

using Json = nlohmann::json;

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
    return LspPosition{.line = static_cast<std::uint32_t>(line),
                       .character = static_cast<std::uint32_t>(character)};
}

LspRange parse_range(const Json& value) {
    if (!value.is_object() || !value.contains("start") || !value.contains("end")) {
        throw std::runtime_error("diagnostic range is missing start or end");
    }
    const std::optional<LspPosition> start = parse_position(value["start"]);
    const std::optional<LspPosition> end = parse_position(value["end"]);
    if (!start || !end) {
        throw std::runtime_error("diagnostic range contains an invalid position");
    }
    return {.start = *start, .end = *end};
}

DiagnosticSeverity parse_severity(const Json& value) {
    if (!value.is_number_integer()) {
        return DiagnosticSeverity::Error;
    }
    switch (value.get<int>()) {
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
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    return {};
}

LspPublishedDiagnostics parse_publish(std::string_view payload) {
    const Json params = Json::parse(payload);
    if (!params.is_object() || !params.contains("uri") || !params["uri"].is_string() ||
        !params.contains("diagnostics") || !params["diagnostics"].is_array()) {
        throw std::runtime_error("publishDiagnostics has an invalid payload");
    }
    LspPublishedDiagnostics published{.uri = params["uri"].get<std::string>(),
                                       .version = std::nullopt,
                                       .diagnostics = {}};
    if (const auto version = params.find("version");
        version != params.end() && !version->is_null()) {
        if (!version->is_number_unsigned()) {
            throw std::runtime_error("publishDiagnostics version is invalid");
        }
        published.version = version->get<RevisionId>();
    }
    published.diagnostics.reserve(params["diagnostics"].size());
    for (const Json& value : params["diagnostics"]) {
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
        [published = std::move(published), failed = std::move(failed)](
            LspNotification notification) mutable {
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
    return Json{{"textDocument",
                 {{"publishDiagnostics",
                   {{"relatedInformation", true},
                    {"versionSupport", true},
                    {"tagSupport", {{"valueSet", Json::array({1, 2})}}}}}}}}
        .dump();
}

} // namespace cind
