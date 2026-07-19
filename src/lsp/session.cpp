#include "lsp/session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <limits>
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
    return LspPosition{static_cast<std::uint32_t>(line),
                       static_cast<std::uint32_t>(character)};
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
        "",          "text",      "method",   "function", "constructor", "field",
        "variable",  "class",     "interface", "module",   "property",    "unit",
        "value",     "enum",      "keyword",  "snippet",   "color",       "file",
        "reference", "folder",    "enum member", "constant", "struct",      "event",
        "operator",  "type parameter",
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

LspCompletionResponse parse_completion_response(const Json& result) {
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
        LspCompletionItem item{
            .label = value["label"].get<std::string>(),
            .insert_text = {},
            .filter_text = value.value("filterText", std::string{}),
            .sort_text = value.value("sortText", std::string{}),
            .kind = value.contains("kind") ? completion_kind(value["kind"]) : std::string{},
            .detail = value.value("detail", std::string{}),
            .edit = std::nullopt,
            .is_snippet = value.value("insertTextFormat", 1) == 2,
            .resolved = true,
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
        response.items.push_back(std::move(item));
    }
    return response;
}

} // namespace

LspSession::LspSession(LspSessionId id, AsyncRuntime& runtime, LspSessionConfig config)
    : id_(id), runtime_(&runtime), config_(std::move(config)) {
    if (!id_.valid() || config_.command.empty() || config_.root.empty() ||
        config_.language_id.empty()) {
        throw std::invalid_argument("LSP session requires an id, command, root, and language");
    }
}

LspSession::~LspSession() {
    stop();
}

LspSessionSnapshot LspSession::snapshot() const {
    return {.id = id_,
            .state = state_,
            .command = config_.command,
            .root = config_.root,
            .pending_requests = pending_.size(),
            .open_documents = documents_.size(),
            .error = error_};
}

std::expected<LspSession::Cancel, std::string>
LspSession::request_completion(LspCompletionRequest request, Completed completed, Failed failed,
                               Cancelled cancelled) {
    if (!completed || !failed) {
        return std::unexpected("LSP completion requires completion and failure callbacks");
    }
    if (state_ == LspSessionState::Failed) {
        return std::unexpected(error_.empty() ? "LSP session failed" : error_);
    }
    if (stopping_) {
        return std::unexpected("LSP session is stopping");
    }
    const std::uint64_t id = ++next_request_;
    auto [found, inserted] = pending_.emplace(
        id, PendingCompletion{.id = id,
                              .request = std::move(request),
                              .completed = std::move(completed),
                              .failed = std::move(failed),
                              .cancelled = std::move(cancelled),
                              .sent = false});
    if (!inserted) {
        return std::unexpected("LSP request id collision");
    }
    if (state_ == LspSessionState::Stopped) {
        start();
    }
    if (state_ == LspSessionState::Ready) {
        if (!send_pending(found->second)) {
            fail("cannot write LSP completion request");
        }
    }
    return Cancel{[this, id] { (void)cancel_request(id); }};
}

bool LspSession::cancel_request(std::uint64_t request) {
    const auto found = pending_.find(request);
    if (found == pending_.end()) {
        return false;
    }
    PendingCompletion pending = std::move(found->second);
    pending_.erase(found);
    if (pending.sent && state_ == LspSessionState::Ready) {
        (void)send_json(Json{{"jsonrpc", "2.0"},
                             {"method", "$/cancelRequest"},
                             {"params", {{"id", request}}}}
                            .dump());
    }
    if (pending.cancelled) {
        pending.cancelled();
    }
    return true;
}

bool LspSession::close_document(std::string_view uri) {
    const auto found = documents_.find(std::string(uri));
    if (found == documents_.end()) {
        return false;
    }
    if (state_ == LspSessionState::Ready) {
        (void)send_json(Json{{"jsonrpc", "2.0"},
                             {"method", "textDocument/didClose"},
                             {"params", {{"textDocument", {{"uri", uri}}}}}}
                            .dump());
    }
    documents_.erase(found);
    return true;
}

void LspSession::stop() noexcept {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    for (auto& [id, pending] : pending_) {
        (void)id;
        if (pending.cancelled) {
            try {
                pending.cancelled();
            } catch (...) {
                continue;
            }
        }
    }
    pending_.clear();
    if (process_.valid()) {
        (void)runtime_->terminate(process_);
    }
    process_ = {};
    state_ = LspSessionState::Stopped;
}

void LspSession::start() {
    state_ = LspSessionState::Starting;
    error_.clear();
    try {
        process_ = runtime_->spawn({
            .file = config_.command,
            .arguments = config_.arguments,
            .working_directory = config_.root,
            .started = [this](AsyncProcessId process) { process_started(process); },
            .standard_output =
                [this](AsyncProcessId, const std::string& bytes) { process_output(bytes); },
            .standard_error = [this](AsyncProcessId, const std::string& bytes) {
                process_error_output(bytes);
            },
            .completed = [this](AsyncProcessResult result) { process_completed(std::move(result)); },
            .cancelled = [this] {
                if (!stopping_) {
                    fail("LSP process was cancelled");
                }
            },
            .failed = [this](const std::exception_ptr& failure) { process_failed(failure); },
        });
    } catch (const std::exception& exception) {
        fail(exception.what());
    }
}

void LspSession::process_started(AsyncProcessId process) {
    if (stopping_ || process != process_) {
        return;
    }
    state_ = LspSessionState::Initializing;
    initialize_request_ = ++next_request_;
    Json initialize{
        {"jsonrpc", "2.0"},
        {"id", initialize_request_},
        {"method", "initialize"},
        {"params",
         {{"processId", nullptr},
          {"clientInfo", {{"name", "cind"}}},
          {"rootUri", path_to_file_uri(config_.root)},
          {"capabilities",
           {{"general", {{"positionEncodings", Json::array({"utf-16"})}}},
            {"textDocument",
             {{"completion",
               {{"completionItem",
                 {{"snippetSupport", false},
                  {"documentationFormat", Json::array({"plaintext", "markdown"})},
                  {"resolveSupport",
                   {{"properties",
                     Json::array({"documentation", "detail", "additionalTextEdits"})}}}}}}}}}}},
          {"workspaceFolders", Json::array({{{"uri", path_to_file_uri(config_.root)},
                                               {"name", config_.root}}})}}},
    };
    if (!send_json(initialize.dump())) {
        fail("cannot write LSP initialize request");
    }
}

void LspSession::process_output(const std::string& bytes) {
    if (stopping_) {
        return;
    }
    std::expected<std::vector<std::string>, std::string> messages = framer_.push(bytes);
    if (!messages) {
        fail(std::move(messages.error()));
        return;
    }
    for (const std::string& message : *messages) {
        handle_message(message);
    }
}

void LspSession::process_error_output(const std::string& bytes) {
    constexpr std::size_t maximum_error = std::size_t{64} * 1024;
    if (standard_error_.size() < maximum_error) {
        standard_error_.append(bytes.substr(0, maximum_error - standard_error_.size()));
    }
}

void LspSession::process_completed(AsyncProcessResult result) {
    process_ = {};
    if (stopping_) {
        return;
    }
    std::string error = std::format("LSP process exited with status {}", result.exit_status);
    if (!standard_error_.empty()) {
        error.append(": ").append(standard_error_);
    }
    fail(std::move(error));
}

void LspSession::process_failed(const std::exception_ptr& failure) {
    process_ = {};
    if (!stopping_) {
        fail(exception_message(failure));
    }
}

void LspSession::handle_message(std::string_view message) {
    Json value;
    try {
        value = Json::parse(message);
    } catch (const std::exception& exception) {
        fail(std::format("cannot parse LSP JSON: {}", exception.what()));
        return;
    }
    if (!value.is_object()) {
        return;
    }
    if (value.contains("method") && value.contains("id")) {
        Json result = nullptr;
        if (value["method"] == "workspace/configuration" && value.contains("params")) {
            const auto items = value["params"].find("items");
            if (items != value["params"].end() && items->is_array()) {
                result = Json::array();
                for (std::size_t index = 0; index < items->size(); ++index) {
                    result.push_back(nullptr);
                }
            }
        }
        (void)send_json(Json{{"jsonrpc", "2.0"}, {"id", value["id"]}, {"result", result}}.dump());
        return;
    }
    if (!value.contains("id") || !value["id"].is_number_unsigned()) {
        return;
    }
    const std::uint64_t id = value["id"].get<std::uint64_t>();
    if (id == initialize_request_) {
        initialize_completed(message);
        return;
    }
    const auto found = pending_.find(id);
    if (found == pending_.end()) {
        return;
    }
    PendingCompletion pending = std::move(found->second);
    pending_.erase(found);
    if (value.contains("error")) {
        const std::string error = value["error"].value("message", "LSP completion failed");
        fail_pending(pending, error);
        return;
    }
    try {
        pending.completed(parse_completion_response(value.value("result", Json{})));
    } catch (const std::exception& exception) {
        fail_pending(pending, exception.what());
    }
}

void LspSession::initialize_completed(std::string_view message) {
    const Json value = Json::parse(message);
    if (value.contains("error")) {
        fail(value["error"].value("message", "LSP initialize failed"));
        return;
    }
    if (!send_json(Json{{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", Json::object()}}
                       .dump())) {
        fail("cannot write LSP initialized notification");
        return;
    }
    state_ = LspSessionState::Ready;
    for (auto& [id, pending] : pending_) {
        (void)id;
        if (!send_pending(pending)) {
            fail("cannot write LSP completion request");
            break;
        }
    }
}

bool LspSession::send_pending(PendingCompletion& pending) {
    if (pending.sent) {
        return true;
    }
    sync_document(pending.request);
    Json context{{"triggerKind", static_cast<int>(pending.request.trigger)}};
    if (!pending.request.trigger_character.empty()) {
        context["triggerCharacter"] = pending.request.trigger_character;
    }
    const Json request{{"jsonrpc", "2.0"},
                       {"id", pending.id},
                       {"method", "textDocument/completion"},
                       {"params",
                        {{"textDocument", {{"uri", pending.request.uri}}},
                         {"position", position_json(lsp_position(pending.request.text,
                                                                  pending.request.caret))},
                         {"context", std::move(context)}}}};
    if (!send_json(request.dump())) {
        return false;
    }
    pending.sent = true;
    return true;
}

void LspSession::sync_document(const LspCompletionRequest& request) {
    const auto found = documents_.find(request.uri);
    const std::int64_t version = static_cast<std::int64_t>(std::min<RevisionId>(
        request.revision, static_cast<RevisionId>(std::numeric_limits<std::int64_t>::max())));
    if (found == documents_.end()) {
        (void)send_json(
            Json{{"jsonrpc", "2.0"},
                 {"method", "textDocument/didOpen"},
                 {"params",
                  {{"textDocument",
                    {{"uri", request.uri},
                     {"languageId", request.language_id},
                     {"version", version},
                     {"text", request.text.to_string()}}}}}}
                .dump());
        documents_.emplace(request.uri, OpenDocument{request.revision});
        return;
    }
    if (found->second.revision == request.revision) {
        return;
    }
    (void)send_json(Json{{"jsonrpc", "2.0"},
                         {"method", "textDocument/didChange"},
                         {"params",
                          {{"textDocument", {{"uri", request.uri}, {"version", version}}},
                           {"contentChanges",
                            Json::array({{{"text", request.text.to_string()}}})}}}}
                        .dump());
    found->second.revision = request.revision;
}

bool LspSession::send_json(const std::string& json) {
    return process_.valid() && runtime_->write(process_, frame_json_rpc(json));
}

void LspSession::fail(std::string error) {
    error_ = std::move(error);
    state_ = LspSessionState::Failed;
    std::vector<PendingCompletion> pending;
    pending.reserve(pending_.size());
    for (auto& [id, request] : pending_) {
        (void)id;
        pending.push_back(std::move(request));
    }
    pending_.clear();
    for (PendingCompletion& request : pending) {
        fail_pending(request, error_);
    }
    if (process_.valid()) {
        (void)runtime_->terminate(process_);
    }
}

void LspSession::fail_pending(const PendingCompletion& pending, std::string error) {
    if (pending.failed) {
        pending.failed(std::move(error));
    }
}

std::string LspSession::exception_message(const std::exception_ptr& failure) {
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& exception) {
        return exception.what();
    } catch (...) {
        return "unknown LSP process failure";
    }
    return "unknown LSP process failure";
}

} // namespace cind
