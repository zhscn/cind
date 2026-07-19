#include "lsp/completion.hpp"
#include "lsp/protocol_json.hpp"

#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cind {

namespace {

using Json = lsp_json::Json;

std::string completion_kind(const Json& value) {
    static constexpr std::string_view names[] = {
        "",         "text",      "method", "function", "constructor",    "field",  "variable",
        "class",    "interface", "module", "property", "unit",           "value",  "enum",
        "keyword",  "snippet",   "color",  "file",     "reference",      "folder", "enum member",
        "constant", "struct",    "event",  "operator", "type parameter",
    };
    const std::optional<std::int64_t> parsed = lsp_json::int64(value);
    if (!parsed) {
        return {};
    }
    const std::int64_t kind = *parsed;
    return kind > 0 && static_cast<std::size_t>(kind) < std::size(names)
               ? std::string(names[static_cast<std::size_t>(kind)])
               : std::string{};
}

std::string documentation_text(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_object()) {
        const Json* found = lsp_json::find(value, "value");
        if (found != nullptr && found->is_string()) {
            return found->get<std::string>();
        }
    }
    return {};
}

LspCompletionItem parse_completion_item(const Json& value, bool resolved) {
    if (!value.is_object() || !value.contains("label") || !value["label"].is_string()) {
        throw std::runtime_error("LSP completion item has no label");
    }
    LspCompletionItem item{
        .label = value["label"].get<std::string>(),
        .insert_text = {},
        .filter_text = lsp_json::value_or(value, "filterText", std::string{}),
        .sort_text = lsp_json::value_or(value, "sortText", std::string{}),
        .kind = value.contains("kind") ? completion_kind(value["kind"]) : std::string{},
        .detail = lsp_json::value_or(value, "detail", std::string{}),
        .edit = std::nullopt,
        .is_snippet = lsp_json::value_or(value, "insertTextFormat", 1) == 2,
        .resolved = resolved,
        .documentation = value.contains("documentation")
                             ? documentation_text(value["documentation"])
                             : std::string{},
        .additional_edits = {},
        .raw = lsp_json::dump(value),
    };
    std::string insertion = lsp_json::value_or(value, "textEditText", std::string{});
    if (insertion.empty()) {
        insertion = lsp_json::value_or(value, "insertText", item.label);
    }
    if (const Json* edit = lsp_json::find(value, "textEdit");
        edit != nullptr && edit->is_object()) {
        const std::string new_text = lsp_json::value_or(*edit, "newText", insertion);
        if (edit->contains("insert") && edit->contains("replace")) {
            const std::optional<LspRange> insert = lsp_json::parse_range((*edit)["insert"]);
            const std::optional<LspRange> replace = lsp_json::parse_range((*edit)["replace"]);
            if (insert && replace) {
                item.edit = LspCompletionEdit{*insert, *replace, new_text};
            }
        } else if (edit->contains("range")) {
            if (const std::optional<LspRange> range = lsp_json::parse_range((*edit)["range"])) {
                item.edit = LspCompletionEdit{*range, *range, new_text};
            }
        }
    }
    if (!item.edit) {
        insertion = lsp_json::value_or(value, "insertText", item.label);
    }
    item.insert_text = std::move(insertion);
    if (const Json* edits = lsp_json::find(value, "additionalTextEdits");
        edits != nullptr && edits->is_array()) {
        for (const Json& edit : edits->get_array()) {
            if (std::optional<LspTextEdit> parsed = lsp_json::parse_text_edit(edit)) {
                item.additional_edits.push_back(std::move(*parsed));
            }
        }
    }
    return item;
}

LspCompletionResponse parse_completion_response(const Json& result, bool resolved) {
    LspCompletionResponse response;
    const Json* items = &result;
    if (result.is_null()) {
        return response;
    }
    if (result.is_object()) {
        response.is_incomplete = lsp_json::value_or(result, "isIncomplete", false);
        const Json* found = lsp_json::find(result, "items");
        if (found == nullptr) {
            throw std::runtime_error("LSP completion list has no items");
        }
        items = found;
    }
    if (!items->is_array()) {
        throw std::runtime_error("LSP completion result is not an array or completion list");
    }
    response.items.reserve(items->size());
    for (const Json& value : items->get_array()) {
        if (!value.is_object() || !value.contains("label") || !value["label"].is_string()) {
            continue;
        }
        response.items.push_back(parse_completion_item(value, resolved));
    }
    return response;
}

void report_failure(const LspCompletionFeature::Failed& failed, std::string message) noexcept {
    try {
        failed(std::move(message));
    } catch (...) {
        return;
    }
}

} // namespace

std::expected<LspCompletionFeature::Cancel, std::string>
LspCompletionFeature::request(LspSession& session, LspCompletionRequest request,
                              Completed completed, Failed failed, Cancelled cancelled) {
    if (!completed || !failed) {
        return std::unexpected("LSP completion requires completion and failure callbacks");
    }
    if (std::expected<void, std::string> synchronized =
            session.synchronize_document({.uri = request.uri,
                                          .language_id = request.language_id,
                                          .revision = request.revision,
                                          .text = request.text});
        !synchronized) {
        return std::unexpected(std::move(synchronized.error()));
    }
    Json context{{"triggerKind", static_cast<int>(request.trigger)}};
    if (!request.trigger_character.empty()) {
        context["triggerCharacter"] = request.trigger_character;
    }
    const Json params{{"textDocument", {{"uri", request.uri}}},
                      {"position", lsp_json::position(lsp_position(request.text, request.caret))},
                      {"context", std::move(context)}};
    Failed parse_failed = failed;
    return session.request(
        {.method = "textDocument/completion", .params = lsp_json::dump(params)},
        [&session, completed = std::move(completed),
         failed = std::move(parse_failed)](const LspResponse& response) mutable {
            try {
                completed(parse_completion_response(lsp_json::parse_or_throw(response.result),
                                                    !supports_resolve(session)));
            } catch (const std::exception& exception) {
                report_failure(failed, std::format("cannot decode LSP completion response: {}",
                                                   exception.what()));
            } catch (...) {
                report_failure(failed, "cannot decode LSP completion response");
            }
        },
        [failed = std::move(failed)](LspResponseError error) mutable {
            report_failure(failed, std::move(error.message));
        },
        std::move(cancelled));
}

std::expected<LspCompletionFeature::Cancel, std::string>
LspCompletionFeature::resolve(LspSession& session, const std::string& item,
                              ResolveCompleted completed, Failed failed, Cancelled cancelled) {
    if (!completed || !failed) {
        return std::unexpected("LSP completion resolve requires completion and failure callbacks");
    }
    if (!supports_resolve(session)) {
        return std::unexpected("LSP server does not support completion item resolve");
    }
    Json payload;
    try {
        payload = lsp_json::parse_or_throw(item);
    } catch (const std::exception& exception) {
        return std::unexpected(
            std::format("invalid LSP completion resolve payload: {}", exception.what()));
    }
    if (!payload.is_object()) {
        return std::unexpected("LSP completion resolve payload is not an object");
    }
    Failed parse_failed = failed;
    return session.request(
        {.method = "completionItem/resolve", .params = lsp_json::dump(payload)},
        [completed = std::move(completed),
         failed = std::move(parse_failed)](const LspResponse& response) mutable {
            try {
                completed(parse_completion_item(lsp_json::parse_or_throw(response.result), true));
            } catch (const std::exception& exception) {
                report_failure(failed,
                               std::format("cannot decode LSP completion resolve response: {}",
                                           exception.what()));
            } catch (...) {
                report_failure(failed, "cannot decode LSP completion resolve response");
            }
        },
        [failed = std::move(failed)](LspResponseError error) mutable {
            report_failure(failed, std::move(error.message));
        },
        std::move(cancelled));
}

bool LspCompletionFeature::supports_resolve(const LspSession& session) {
    return session.capability_boolean({"completionProvider", "resolveProvider"});
}

std::string LspCompletionFeature::client_capabilities() {
    Json completion_item = Json::object_t{
        {"snippetSupport", false},
        {"documentationFormat", Json::array_t{"plaintext", "markdown"}},
        {"resolveSupport", Json::object_t{{"properties", Json::array_t{"documentation", "detail",
                                                                       "additionalTextEdits"}}}},
    };
    return lsp_json::dump(Json::object_t{
        {"textDocument",
         Json::object_t{
             {"completion", Json::object_t{{"completionItem", std::move(completion_item)}}}}},
    });
}

} // namespace cind
