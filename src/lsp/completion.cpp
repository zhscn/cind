#include "lsp/completion.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
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
    if (!start || !end) {
        return std::nullopt;
    }
    return LspRange{*start, *end};
}

std::string completion_kind(const Json& value) {
    static constexpr std::string_view names[] = {
        "",         "text",      "method", "function", "constructor",    "field",  "variable",
        "class",    "interface", "module", "property", "unit",           "value",  "enum",
        "keyword",  "snippet",   "color",  "file",     "reference",      "folder", "enum member",
        "constant", "struct",    "event",  "operator", "type parameter",
    };
    if (!value.is_number_integer()) {
        return {};
    }
    const std::int64_t kind = value.get<std::int64_t>();
    return kind > 0 && static_cast<std::size_t>(kind) < std::size(names)
               ? std::string(names[static_cast<std::size_t>(kind)])
               : std::string{};
}

std::string documentation_text(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_object()) {
        const auto found = value.find("value");
        if (found != value.end() && found->is_string()) {
            return found->get<std::string>();
        }
    }
    return {};
}

std::optional<LspTextEdit> parse_text_edit(const Json& value) {
    if (!value.is_object() || !value.contains("range") || !value.contains("newText") ||
        !value["newText"].is_string()) {
        return std::nullopt;
    }
    const std::optional<LspRange> range = parse_range(value["range"]);
    if (!range) {
        return std::nullopt;
    }
    return LspTextEdit{.range = *range, .new_text = value["newText"].get<std::string>()};
}

LspCompletionItem parse_completion_item(const Json& value, bool resolved) {
    if (!value.is_object() || !value.contains("label") || !value["label"].is_string()) {
        throw std::runtime_error("LSP completion item has no label");
    }
    LspCompletionItem item{
        .label = value["label"].get<std::string>(),
        .insert_text = {},
        .filter_text = value.value("filterText", std::string{}),
        .sort_text = value.value("sortText", std::string{}),
        .kind = value.contains("kind") ? completion_kind(value["kind"]) : std::string{},
        .detail = value.value("detail", std::string{}),
        .edit = std::nullopt,
        .is_snippet = value.value("insertTextFormat", 1) == 2,
        .resolved = resolved,
        .documentation = value.contains("documentation")
                             ? documentation_text(value["documentation"])
                             : std::string{},
        .additional_edits = {},
        .raw = value.dump(),
    };
    std::string insertion = value.value("textEditText", std::string{});
    if (insertion.empty()) {
        insertion = value.value("insertText", item.label);
    }
    if (const auto edit = value.find("textEdit"); edit != value.end() && edit->is_object()) {
        const std::string new_text = edit->value("newText", insertion);
        if (edit->contains("insert") && edit->contains("replace")) {
            const std::optional<LspRange> insert = parse_range((*edit)["insert"]);
            const std::optional<LspRange> replace = parse_range((*edit)["replace"]);
            if (insert && replace) {
                item.edit = LspCompletionEdit{*insert, *replace, new_text};
            }
        } else if (edit->contains("range")) {
            if (const std::optional<LspRange> range = parse_range((*edit)["range"])) {
                item.edit = LspCompletionEdit{*range, *range, new_text};
            }
        }
    }
    if (!item.edit) {
        insertion = value.value("insertText", item.label);
    }
    item.insert_text = std::move(insertion);
    if (const auto edits = value.find("additionalTextEdits");
        edits != value.end() && edits->is_array()) {
        for (const Json& edit : *edits) {
            if (std::optional<LspTextEdit> parsed = parse_text_edit(edit)) {
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
        response.is_incomplete = result.value("isIncomplete", false);
        const auto found = result.find("items");
        if (found == result.end()) {
            throw std::runtime_error("LSP completion list has no items");
        }
        items = &*found;
    }
    if (!items->is_array()) {
        throw std::runtime_error("LSP completion result is not an array or completion list");
    }
    response.items.reserve(items->size());
    for (const Json& value : *items) {
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
                      {"position", position_json(lsp_position(request.text, request.caret))},
                      {"context", std::move(context)}};
    Failed parse_failed = failed;
    return session.request(
        {.method = "textDocument/completion", .params = params.dump()},
        [&session, completed = std::move(completed),
         failed = std::move(parse_failed)](const LspResponse& response) mutable {
            try {
                completed(parse_completion_response(Json::parse(response.result),
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
        payload = Json::parse(item);
    } catch (const std::exception& exception) {
        return std::unexpected(
            std::format("invalid LSP completion resolve payload: {}", exception.what()));
    }
    if (!payload.is_object()) {
        return std::unexpected("LSP completion resolve payload is not an object");
    }
    Failed parse_failed = failed;
    return session.request(
        {.method = "completionItem/resolve", .params = payload.dump()},
        [completed = std::move(completed),
         failed = std::move(parse_failed)](const LspResponse& response) mutable {
            try {
                completed(parse_completion_item(Json::parse(response.result), true));
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
    Json completion_item{
        {"snippetSupport", false},
        {"documentationFormat", Json::array({"plaintext", "markdown"})},
        {"resolveSupport",
         {{"properties", Json::array({"documentation", "detail", "additionalTextEdits"})}}}};
    return Json{
        {"textDocument", {{"completion", {{"completionItem", std::move(completion_item)}}}}}}
        .dump();
}

} // namespace cind
