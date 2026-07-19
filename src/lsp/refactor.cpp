#include "lsp/refactor.hpp"

#include "lsp/protocol_json.hpp"

#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cind {

namespace {

using Json = lsp_json::Json;

std::vector<LspTextEdit> parse_text_edits(const Json& values) {
    if (!values.is_array()) {
        throw std::runtime_error("workspace edit text edits are not an array");
    }
    std::vector<LspTextEdit> edits;
    edits.reserve(values.size());
    for (const Json& value : values.get_array()) {
        std::optional<LspTextEdit> edit = lsp_json::parse_text_edit(value);
        if (!edit) {
            throw std::runtime_error("workspace edit contains an invalid text edit");
        }
        edits.push_back(std::move(*edit));
    }
    return edits;
}

std::string resource_from_uri(const Json& value) {
    if (!value.is_string()) {
        throw std::runtime_error("workspace edit contains an invalid document URI");
    }
    std::expected<std::string, std::string> resource =
        file_uri_to_path(value.get<std::string>());
    if (!resource) {
        throw std::runtime_error(resource.error());
    }
    return std::move(*resource);
}

LspWorkspaceEdit parse_workspace_edit(const Json& value) {
    if (!value.is_object()) {
        throw std::runtime_error("workspace edit is not an object");
    }
    LspWorkspaceEdit result;
    if (const Json* changes = lsp_json::find(value, "changes")) {
        if (!changes->is_object()) {
            throw std::runtime_error("workspace edit changes are not an object");
        }
        for (const auto& [uri, edits] : changes->get_object()) {
            result.documents.push_back(
                {.resource = resource_from_uri(Json(uri)),
                 .version = std::nullopt,
                 .edits = parse_text_edits(edits)});
        }
    }
    if (const Json* changes = lsp_json::find(value, "documentChanges")) {
        if (!changes->is_array()) {
            throw std::runtime_error("workspace edit documentChanges are not an array");
        }
        for (const Json& change : changes->get_array()) {
            if (!change.is_object()) {
                throw std::runtime_error("workspace edit contains an invalid document change");
            }
            const Json* document = lsp_json::find(change, "textDocument");
            const Json* edits = lsp_json::find(change, "edits");
            if (document == nullptr || edits == nullptr) {
                result.has_resource_operations = true;
                continue;
            }
            if (!document->is_object() || !document->contains("uri")) {
                throw std::runtime_error("workspace edit has an invalid text document");
            }
            std::optional<RevisionId> version;
            if (const Json* declared = lsp_json::find(*document, "version");
                declared != nullptr && !declared->is_null()) {
                const std::optional<std::uint64_t> parsed = lsp_json::uint64(*declared);
                if (!parsed) {
                    throw std::runtime_error("workspace edit has an invalid document version");
                }
                version = static_cast<RevisionId>(*parsed);
            }
            result.documents.push_back(
                {.resource = resource_from_uri((*document)["uri"]),
                 .version = version,
                 .edits = parse_text_edits(*edits)});
        }
    }
    return result;
}

std::optional<LspCommand> parse_command(const Json& value) {
    if (!value.is_object() || !value.contains("title") || !value["title"].is_string() ||
        !value.contains("command") || !value["command"].is_string()) {
        return std::nullopt;
    }
    return LspCommand{.title = value["title"].get<std::string>(),
                      .command = value["command"].get<std::string>(),
                      .arguments = value.contains("arguments") ? lsp_json::dump(value["arguments"])
                                                               : "[]"};
}

LspCodeAction parse_code_action(const Json& value, bool resolved) {
    if (!value.is_object() || !value.contains("title") || !value["title"].is_string()) {
        throw std::runtime_error("code action has no title");
    }
    LspCodeAction action{.title = value["title"].get<std::string>(),
                         .kind = lsp_json::value_or(value, "kind", std::string{}),
                         .preferred = lsp_json::value_or(value, "isPreferred", false),
                         .disabled_reason = {},
                         .edit = std::nullopt,
                         .command = std::nullopt,
                         .resolved = resolved,
                         .raw = lsp_json::dump(value)};
    if (const Json* disabled = lsp_json::find(value, "disabled");
        disabled != nullptr && disabled->is_object()) {
        action.disabled_reason = lsp_json::value_or(*disabled, "reason", std::string{});
    }
    if (const Json* edit = lsp_json::find(value, "edit")) {
        action.edit = parse_workspace_edit(*edit);
    }
    if (const Json* command = lsp_json::find(value, "command")) {
        action.command = parse_command(*command);
        if (!action.command) {
            throw std::runtime_error("code action has an invalid command");
        }
    }
    return action;
}

std::vector<LspCodeAction> parse_code_actions(const Json& value, bool resolved) {
    if (value.is_null()) {
        return {};
    }
    if (!value.is_array()) {
        throw std::runtime_error("code action result is not an array");
    }
    std::vector<LspCodeAction> actions;
    actions.reserve(value.size());
    for (const Json& candidate : value.get_array()) {
        if (std::optional<LspCommand> command = parse_command(candidate)) {
            actions.push_back({.title = command->title,
                               .kind = {},
                               .preferred = false,
                               .disabled_reason = {},
                               .edit = std::nullopt,
                               .command = std::move(command),
                               .resolved = true,
                               .raw = lsp_json::dump(candidate)});
        } else {
            actions.push_back(parse_code_action(candidate, resolved));
        }
    }
    return actions;
}

void report_failure(const LspRefactorFeature::Failed& failed, std::string message) noexcept {
    try {
        failed(std::move(message));
    } catch (...) {
        return;
    }
}

std::expected<void, std::string> synchronize(LspSession& session,
                                             const LspRefactorRequest& request) {
    return session.synchronize_document({.uri = request.uri,
                                         .language_id = request.language_id,
                                         .revision = request.revision,
                                         .text = request.text});
}

Json request_position(const LspRefactorRequest& request) {
    return {{"textDocument", {{"uri", request.uri}}},
            {"position", lsp_json::position(lsp_position(request.text, request.caret))}};
}

bool capability_enabled(const LspSession& session,
                        std::initializer_list<std::string_view> path) {
    const std::optional<std::string> value = session.capability(path);
    if (!value) {
        return false;
    }
    try {
        const Json parsed = lsp_json::parse_or_throw(*value, "LSP capability");
        return !parsed.is_null() && (!parsed.is_boolean() || parsed.get<bool>());
    } catch (...) {
        return false;
    }
}

} // namespace

std::expected<LspRefactorFeature::Cancel, std::string>
LspRefactorFeature::rename(LspSession& session, const LspRefactorRequest& request,
                           const std::string& new_name,
                           RenameCompleted completed, Failed failed, Cancelled cancelled) {
    if (!completed || !failed || new_name.empty()) {
        return std::unexpected("LSP rename requires a name and completion callbacks");
    }
    if (std::expected<void, std::string> synchronized = synchronize(session, request);
        !synchronized) {
        return std::unexpected(std::move(synchronized.error()));
    }
    Json params = request_position(request);
    params["newName"] = new_name;
    Failed parse_failed = failed;
    return session.request(
        {.method = "textDocument/rename", .params = lsp_json::dump(params)},
        [completed = std::move(completed), failed = std::move(parse_failed)](
            const LspResponse& response) mutable {
            try {
                const Json result = lsp_json::parse_or_throw(response.result, "LSP rename result");
                completed(result.is_null() ? std::nullopt
                                           : std::optional(parse_workspace_edit(result)));
            } catch (const std::exception& exception) {
                report_failure(failed,
                               std::format("cannot decode LSP rename response: {}", exception.what()));
            } catch (...) {
                report_failure(failed, "cannot decode LSP rename response");
            }
        },
        [failed = std::move(failed)](LspResponseError error) mutable {
            report_failure(failed, std::move(error.message));
        },
        std::move(cancelled));
}

std::expected<LspRefactorFeature::Cancel, std::string>
LspRefactorFeature::code_actions(LspSession& session, const LspRefactorRequest& request,
                                 CodeActionsCompleted completed, Failed failed,
                                 Cancelled cancelled) {
    if (!completed || !failed) {
        return std::unexpected("LSP code actions require completion callbacks");
    }
    if (std::expected<void, std::string> synchronized = synchronize(session, request);
        !synchronized) {
        return std::unexpected(std::move(synchronized.error()));
    }
    const LspRange range{.start = lsp_position(request.text, request.range.start),
                         .end = lsp_position(request.text, request.range.end)};
    Json params{{"textDocument", {{"uri", request.uri}}},
                {"range",
                 {{"start", lsp_json::position(range.start)},
                  {"end", lsp_json::position(range.end)}}},
                {"context", {{"diagnostics", Json::array_t{}}, {"triggerKind", 1U}}}};
    const bool resolved = !supports_code_action_resolve(session);
    Failed parse_failed = failed;
    return session.request(
        {.method = "textDocument/codeAction", .params = lsp_json::dump(params)},
        [completed = std::move(completed), failed = std::move(parse_failed), resolved](
            const LspResponse& response) mutable {
            try {
                completed(parse_code_actions(
                    lsp_json::parse_or_throw(response.result, "LSP code action result"), resolved));
            } catch (const std::exception& exception) {
                report_failure(failed, std::format("cannot decode LSP code action response: {}",
                                                   exception.what()));
            } catch (...) {
                report_failure(failed, "cannot decode LSP code action response");
            }
        },
        [failed = std::move(failed)](LspResponseError error) mutable {
            report_failure(failed, std::move(error.message));
        },
        std::move(cancelled));
}

std::expected<LspRefactorFeature::Cancel, std::string>
LspRefactorFeature::resolve_code_action(LspSession& session, const std::string& raw,
                                        CodeActionResolved completed, Failed failed,
                                        Cancelled cancelled) {
    if (!completed || !failed || !supports_code_action_resolve(session)) {
        return std::unexpected("LSP code action resolve is unavailable");
    }
    Json params;
    try {
        params = lsp_json::parse_or_throw(raw, "code action payload");
    } catch (const std::exception& exception) {
        return std::unexpected(std::format("invalid code action payload: {}", exception.what()));
    }
    Failed parse_failed = failed;
    return session.request(
        {.method = "codeAction/resolve", .params = lsp_json::dump(params)},
        [completed = std::move(completed), failed = std::move(parse_failed)](
            const LspResponse& response) mutable {
            try {
                completed(parse_code_action(
                    lsp_json::parse_or_throw(response.result, "resolved code action"), true));
            } catch (const std::exception& exception) {
                report_failure(failed, std::format("cannot decode resolved code action: {}",
                                                   exception.what()));
            } catch (...) {
                report_failure(failed, "cannot decode resolved code action");
            }
        },
        [failed = std::move(failed)](LspResponseError error) mutable {
            report_failure(failed, std::move(error.message));
        },
        std::move(cancelled));
}

bool LspRefactorFeature::supports_rename(const LspSession& session) {
    return capability_enabled(session, {"renameProvider"});
}

bool LspRefactorFeature::supports_code_actions(const LspSession& session) {
    return capability_enabled(session, {"codeActionProvider"});
}

bool LspRefactorFeature::supports_code_action_resolve(const LspSession& session) {
    return session.capability_boolean({"codeActionProvider", "resolveProvider"});
}

std::string LspRefactorFeature::client_capabilities() {
    Json workspace_edit = Json::object_t {
        {"documentChanges", true},
        {"resourceOperations", Json::array_t {"create", "rename", "delete"}},
    };
    Json code_action = Json::object_t {
        {"codeActionLiteralSupport",
         Json::object_t {{"codeActionKind",
                          Json::object_t {{"valueSet", Json::array_t {}}}}}},
        {"resolveSupport",
         Json::object_t {{"properties", Json::array_t {"edit", "command"}}}},
    };
    return lsp_json::dump(Json::object_t {
        {"workspace", Json::object_t {{"workspaceEdit", std::move(workspace_edit)}}},
        {"textDocument",
         Json::object_t {
             {"rename", Json::object_t {{"prepareSupport", false}}},
             {"codeAction", std::move(code_action)},
         }},
    });
}

} // namespace cind
