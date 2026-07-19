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

std::expected<Json, std::string> parse_payload(std::string_view payload,
                                               std::string_view description) {
    try {
        return Json::parse(payload);
    } catch (const std::exception& exception) {
        return std::unexpected(
            std::format("{} is not valid JSON: {}", description, exception.what()));
    }
}

LspResponseError response_error(const Json& value, std::string_view fallback) {
    LspResponseError error{.code = std::nullopt, .message = std::string(fallback), .data = "null"};
    if (!value.is_object()) {
        return error;
    }
    if (const auto code = value.find("code"); code != value.end() && code->is_number_integer()) {
        error.code = code->get<std::int64_t>();
    }
    if (const auto message = value.find("message");
        message != value.end() && message->is_string()) {
        error.message = message->get<std::string>();
    }
    if (const auto data = value.find("data"); data != value.end()) {
        error.data = data->dump();
    }
    return error;
}

Json error_json(const LspResponseError& error) {
    Json value{{"code", error.code.value_or(-32603)}, {"message", error.message}};
    if (error.data != "null") {
        if (std::expected<Json, std::string> data = parse_payload(error.data, "LSP error data")) {
            value["data"] = std::move(*data);
        }
    }
    return value;
}

Json workspace_configuration_result(const Json& params) {
    Json result = Json::array();
    if (!params.is_object()) {
        return result;
    }
    const auto items = params.find("items");
    if (items == params.end() || !items->is_array()) {
        return result;
    }
    for (std::size_t index = 0; index < items->size(); ++index) {
        result.push_back(nullptr);
    }
    return result;
}

std::expected<void, std::string> merge_capabilities(Json& target, const Json& source,
                                                    std::string_view path = {}) {
    for (auto member = source.begin(); member != source.end(); ++member) {
        const std::string member_path =
            path.empty() ? member.key() : std::format("{}.{}", path, member.key());
        if (!target.contains(member.key())) {
            target[member.key()] = member.value();
            continue;
        }
        Json& existing = target[member.key()];
        if (existing.is_object() && member->is_object()) {
            if (std::expected<void, std::string> merged =
                    merge_capabilities(existing, *member, member_path);
                !merged) {
                return merged;
            }
            continue;
        }
        if (existing.is_array() && member->is_array()) {
            for (const Json& value : *member) {
                if (std::ranges::find(existing, value) == existing.end()) {
                    existing.push_back(value);
                }
            }
            continue;
        }
        if (existing != *member) {
            return std::unexpected(
                std::format("conflicting LSP client capability {}", member_path));
        }
    }
    return {};
}

} // namespace

LspSession::LspSession(LspSessionId id, AsyncRuntime& runtime, LspSessionConfig config)
    : id_(id), runtime_(&runtime), config_(std::move(config)) {
    if (!id_.valid() || config_.command.empty() || config_.root.empty() ||
        config_.language_id.empty()) {
        throw std::invalid_argument("LSP session requires an id, command, root, and language");
    }
    Json capabilities = Json::object();
    for (const std::string& fragment : config_.client_capabilities) {
        const std::expected<Json, std::string> parsed =
            parse_payload(fragment, "LSP client capability fragment");
        if (!parsed || !parsed->is_object()) {
            throw std::invalid_argument(parsed ? "LSP client capability fragment must be an object"
                                               : parsed.error());
        }
        if (std::expected<void, std::string> merged = merge_capabilities(capabilities, *parsed);
            !merged) {
            throw std::invalid_argument(merged.error());
        }
    }
    client_capabilities_ = capabilities.dump();
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
            .server_capabilities = server_capabilities_,
            .error = error_};
}

std::expected<LspSession::Cancel, std::string>
LspSession::request(LspRequest request, Completed completed, Failed failed, Cancelled cancelled) {
    if (request.method.empty()) {
        return std::unexpected("LSP request method must not be empty");
    }
    if (!completed || !failed) {
        return std::unexpected("LSP request requires completion and failure callbacks");
    }
    if (std::expected<Json, std::string> params =
            parse_payload(request.params, "LSP request params");
        !params) {
        return std::unexpected(std::move(params.error()));
    }
    if (state_ == LspSessionState::Failed) {
        return std::unexpected(error_.empty() ? "LSP session failed" : error_);
    }
    if (stopping_) {
        return std::unexpected("LSP session is stopping");
    }
    if (state_ == LspSessionState::Stopped) {
        start();
    }
    if (state_ == LspSessionState::Failed) {
        return std::unexpected(error_.empty() ? "LSP session failed" : error_);
    }
    const std::uint64_t id = ++next_request_;
    auto [found, inserted] = pending_.emplace(id, PendingRequest{.id = id,
                                                                 .request = std::move(request),
                                                                 .completed = std::move(completed),
                                                                 .failed = std::move(failed),
                                                                 .cancelled = std::move(cancelled),
                                                                 .sent = false});
    if (!inserted) {
        return std::unexpected("LSP request id collision");
    }
    if (state_ == LspSessionState::Ready && !send_pending(found->second)) {
        const std::string error =
            std::format("cannot write LSP request {}", found->second.request.method);
        fail(error);
    }
    return Cancel{[this, id] { (void)cancel_request(id); }};
}

std::expected<void, std::string> LspSession::notify(LspNotification notification) {
    if (notification.method.empty()) {
        return std::unexpected("LSP notification method must not be empty");
    }
    std::expected<Json, std::string> params =
        parse_payload(notification.params, "LSP notification params");
    if (!params) {
        return std::unexpected(std::move(params.error()));
    }
    if (state_ != LspSessionState::Ready || !process_.valid()) {
        return std::unexpected(error_.empty() ? "LSP session is not ready" : error_);
    }
    if (!send_json(Json{{"jsonrpc", "2.0"},
                        {"method", std::move(notification.method)},
                        {"params", std::move(*params)}}
                       .dump())) {
        fail("cannot write LSP notification");
        return std::unexpected(error_);
    }
    return {};
}

std::expected<void, std::string> LspSession::synchronize_document(LspDocumentSnapshot document) {
    if (document.uri.empty() || document.language_id.empty()) {
        return std::unexpected("LSP document requires a URI and language id");
    }
    if (state_ == LspSessionState::Failed) {
        return std::unexpected(error_.empty() ? "LSP session failed" : error_);
    }
    if (stopping_) {
        return std::unexpected("LSP session is stopping");
    }
    auto found = documents_.find(document.uri);
    if (found == documents_.end()) {
        found =
            documents_
                .emplace(document.uri, OpenDocument{.language_id = std::move(document.language_id),
                                                    .revision = document.revision,
                                                    .text = std::move(document.text),
                                                    .published = false})
                .first;
    } else if (document.revision < found->second.revision) {
        return std::unexpected("cannot synchronize a stale LSP document revision");
    } else if (found->second.revision == document.revision &&
               found->second.language_id == document.language_id) {
        return {};
    } else {
        bool published = found->second.published;
        if (published && found->second.language_id != document.language_id) {
            std::expected<void, std::string> closed =
                notify({.method = "textDocument/didClose",
                        .params = Json{{"textDocument", {{"uri", document.uri}}}}.dump()});
            if (!closed) {
                return std::unexpected(std::move(closed.error()));
            }
            published = false;
        }
        found->second = {.language_id = std::move(document.language_id),
                         .revision = document.revision,
                         .text = std::move(document.text),
                         .published = published};
    }
    if (state_ == LspSessionState::Stopped) {
        start();
    }
    if (state_ == LspSessionState::Failed) {
        return std::unexpected(error_.empty() ? "LSP session failed" : error_);
    }
    if (state_ == LspSessionState::Ready && !publish_document(found->first, found->second)) {
        fail("cannot synchronize LSP document");
        return std::unexpected(error_);
    }
    return {};
}

std::optional<std::string>
LspSession::capability(std::initializer_list<std::string_view> path) const {
    try {
        const Json capabilities = Json::parse(server_capabilities_);
        const Json* value = &capabilities;
        for (const std::string_view segment : path) {
            if (!value->is_object()) {
                return std::nullopt;
            }
            const auto found = value->find(std::string(segment));
            if (found == value->end()) {
                return std::nullopt;
            }
            value = &*found;
        }
        return value->dump();
    } catch (...) {
        return std::nullopt;
    }
}

bool LspSession::capability_boolean(std::initializer_list<std::string_view> path,
                                    bool fallback) const {
    const std::optional<std::string> value = capability(path);
    if (!value) {
        return fallback;
    }
    try {
        const Json parsed = Json::parse(*value);
        return parsed.is_boolean() ? parsed.get<bool>() : fallback;
    } catch (...) {
        return fallback;
    }
}

void LspSession::set_notification_handler(std::string method, NotificationHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument("LSP notification handler requires a method and callback");
    }
    notification_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

bool LspSession::clear_notification_handler(std::string_view method) {
    return notification_handlers_.erase(std::string(method)) != 0;
}

void LspSession::set_server_request_handler(std::string method, ServerRequestHandler handler) {
    if (method.empty() || !handler) {
        throw std::invalid_argument("LSP server request handler requires a method and callback");
    }
    server_request_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

bool LspSession::clear_server_request_handler(std::string_view method) {
    return server_request_handlers_.erase(std::string(method)) != 0;
}

bool LspSession::cancel_request(std::uint64_t request) {
    const auto found = pending_.find(request);
    if (found == pending_.end()) {
        return false;
    }
    PendingRequest pending = std::move(found->second);
    pending_.erase(found);
    if (pending.sent && state_ == LspSessionState::Ready) {
        if (!send_json(Json{
                {"jsonrpc", "2.0"}, {"method", "$/cancelRequest"}, {"params", {{"id", request}}}}
                           .dump())) {
            fail("cannot write LSP cancellation notification");
        }
    }
    if (pending.cancelled) {
        try {
            pending.cancelled();
        } catch (const std::exception& exception) {
            error_ = std::format("LSP cancellation callback failed: {}", exception.what());
        } catch (...) {
            error_ = "LSP cancellation callback failed";
        }
    }
    return true;
}

bool LspSession::close_document(std::string_view uri) {
    const auto found = documents_.find(std::string(uri));
    if (found == documents_.end()) {
        return false;
    }
    if (found->second.published && state_ == LspSessionState::Ready) {
        if (std::expected<void, std::string> sent =
                notify({.method = "textDocument/didClose",
                        .params = Json{{"textDocument", {{"uri", uri}}}}.dump()});
            !sent) {
            documents_.erase(found);
            return true;
        }
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
            .standard_output = [this](AsyncProcessId,
                                      const std::string& bytes) { process_output(bytes); },
            .standard_error = [this](AsyncProcessId,
                                     const std::string& bytes) { process_error_output(bytes); },
            .completed =
                [this](AsyncProcessResult result) { process_completed(std::move(result)); },
            .cancelled =
                [this] {
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
    Json capabilities = Json::parse(client_capabilities_);
    capabilities["general"]["positionEncodings"] = Json::array({"utf-16"});
    const Json initialize{
        {"jsonrpc", "2.0"},
        {"id", initialize_request_},
        {"method", "initialize"},
        {"params",
         {{"processId", nullptr},
          {"clientInfo", {{"name", "cind"}}},
          {"rootUri", path_to_file_uri(config_.root)},
          {"capabilities", std::move(capabilities)},
          {"workspaceFolders",
           Json::array({{{"uri", path_to_file_uri(config_.root)}, {"name", config_.root}}})}}},
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
        if (state_ == LspSessionState::Failed) {
            return;
        }
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
    if (const auto method = value.find("method"); method != value.end() && method->is_string()) {
        const std::string params = value.value("params", Json(nullptr)).dump();
        if (const auto id = value.find("id"); id != value.end()) {
            handle_server_request(id->dump(), method->get<std::string>(), params);
        } else {
            handle_notification(method->get<std::string>(), params);
        }
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
    PendingRequest pending = std::move(found->second);
    pending_.erase(found);
    if (const auto error = value.find("error"); error != value.end()) {
        fail_pending(pending, response_error(*error, "LSP request failed"));
        return;
    }
    try {
        pending.completed({.result = value.value("result", Json(nullptr)).dump()});
    } catch (const std::exception& exception) {
        error_ = std::format("LSP response callback failed: {}", exception.what());
    } catch (...) {
        error_ = "LSP response callback failed";
    }
}

void LspSession::initialize_completed(std::string_view message) {
    const Json value = Json::parse(message);
    if (const auto error = value.find("error"); error != value.end()) {
        fail(response_error(*error, "LSP initialize failed").message);
        return;
    }
    const Json result = value.value("result", Json::object());
    server_capabilities_ = result.value("capabilities", Json::object()).dump();
    if (!send_json(Json{{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", Json::object()}}
                       .dump())) {
        fail("cannot write LSP initialized notification");
        return;
    }
    state_ = LspSessionState::Ready;
    if (!flush_documents()) {
        fail("cannot synchronize LSP documents after initialization");
        return;
    }
    std::vector<std::uint64_t> requests;
    requests.reserve(pending_.size());
    for (const auto& [id, pending] : pending_) {
        (void)pending;
        requests.push_back(id);
    }
    for (const std::uint64_t id : requests) {
        const auto pending = pending_.find(id);
        if (pending != pending_.end() && !send_pending(pending->second)) {
            fail(std::format("cannot write LSP request {}", pending->second.request.method));
            return;
        }
    }
}

void LspSession::handle_server_request(std::string_view id, std::string_view method,
                                       std::string_view params) {
    const Json response_id = Json::parse(id);
    const auto reply = [this](const Json& response) {
        if (send_json(response.dump())) {
            return true;
        }
        fail("cannot write LSP server request response");
        return false;
    };
    const auto handler = server_request_handlers_.find(std::string(method));
    if (method == "workspace/configuration" && handler == server_request_handlers_.end()) {
        const Json request_params = Json::parse(params);
        (void)reply(Json{{"jsonrpc", "2.0"},
                         {"id", response_id},
                         {"result", workspace_configuration_result(request_params)}});
        return;
    }
    if (handler == server_request_handlers_.end()) {
        const LspResponseError error{.code = -32601,
                                     .message =
                                         std::format("unsupported server request {}", method),
                                     .data = "null"};
        (void)reply(Json{{"jsonrpc", "2.0"}, {"id", response_id}, {"error", error_json(error)}});
        return;
    }
    try {
        ServerRequestHandler callback = handler->second;
        std::expected<std::string, LspResponseError> response =
            callback({.method = std::string(method), .params = std::string(params)});
        if (!response) {
            (void)reply(Json{
                {"jsonrpc", "2.0"}, {"id", response_id}, {"error", error_json(response.error())}});
            return;
        }
        std::expected<Json, std::string> result = parse_payload(*response, "LSP server result");
        if (!result) {
            const LspResponseError error{
                .code = -32603, .message = std::move(result.error()), .data = "null"};
            (void)reply(
                Json{{"jsonrpc", "2.0"}, {"id", response_id}, {"error", error_json(error)}});
            return;
        }
        (void)reply(Json{{"jsonrpc", "2.0"}, {"id", response_id}, {"result", std::move(*result)}});
    } catch (const std::exception& exception) {
        const LspResponseError error{.code = -32603, .message = exception.what(), .data = "null"};
        (void)reply(Json{{"jsonrpc", "2.0"}, {"id", response_id}, {"error", error_json(error)}});
    } catch (...) {
        const LspResponseError error{
            .code = -32603, .message = "server request handler failed", .data = "null"};
        (void)reply(Json{{"jsonrpc", "2.0"}, {"id", response_id}, {"error", error_json(error)}});
    }
}

void LspSession::handle_notification(std::string_view method, std::string_view params) {
    const auto handler = notification_handlers_.find(std::string(method));
    if (handler == notification_handlers_.end()) {
        return;
    }
    try {
        NotificationHandler callback = handler->second;
        callback({.method = std::string(method), .params = std::string(params)});
    } catch (const std::exception& exception) {
        error_ = std::format("LSP notification handler failed: {}", exception.what());
    } catch (...) {
        error_ = "LSP notification handler failed";
    }
}

bool LspSession::send_pending(PendingRequest& pending) {
    if (pending.sent) {
        return true;
    }
    const Json request{{"jsonrpc", "2.0"},
                       {"id", pending.id},
                       {"method", pending.request.method},
                       {"params", Json::parse(pending.request.params)}};
    if (!send_json(request.dump())) {
        return false;
    }
    pending.sent = true;
    return true;
}

bool LspSession::publish_document(std::string_view uri, OpenDocument& document) {
    const std::int64_t version = static_cast<std::int64_t>(std::min<RevisionId>(
        document.revision, static_cast<RevisionId>(std::numeric_limits<std::int64_t>::max())));
    if (!document.published) {
        const Json params{{"textDocument",
                           {{"uri", uri},
                            {"languageId", document.language_id},
                            {"version", version},
                            {"text", document.text.to_string()}}}};
        if (!send_json(
                Json{{"jsonrpc", "2.0"}, {"method", "textDocument/didOpen"}, {"params", params}}
                    .dump())) {
            return false;
        }
        document.published = true;
        return true;
    }
    const Json params{{"textDocument", {{"uri", uri}, {"version", version}}},
                      {"contentChanges", Json::array({{{"text", document.text.to_string()}}})}};
    return send_json(
        Json{{"jsonrpc", "2.0"}, {"method", "textDocument/didChange"}, {"params", params}}.dump());
}

bool LspSession::flush_documents() {
    for (auto& [uri, document] : documents_) {
        if (!publish_document(uri, document)) {
            return false;
        }
    }
    return true;
}

bool LspSession::send_json(const std::string& json) {
    return process_.valid() && runtime_->write(process_, frame_json_rpc(json));
}

void LspSession::fail(std::string error) {
    if (state_ == LspSessionState::Failed) {
        return;
    }
    error_ = std::move(error);
    state_ = LspSessionState::Failed;
    std::vector<PendingRequest> pending;
    pending.reserve(pending_.size());
    for (auto& [id, request] : pending_) {
        (void)id;
        pending.push_back(std::move(request));
    }
    pending_.clear();
    for (PendingRequest& request : pending) {
        fail_pending(request, {.code = std::nullopt, .message = error_, .data = "null"});
    }
    if (process_.valid()) {
        (void)runtime_->terminate(process_);
    }
}

void LspSession::fail_pending(const PendingRequest& pending, LspResponseError error) {
    if (pending.failed) {
        try {
            pending.failed(std::move(error));
        } catch (const std::exception& exception) {
            if (state_ != LspSessionState::Failed) {
                error_ = std::format("LSP failure callback failed: {}", exception.what());
            }
        } catch (...) {
            if (state_ != LspSessionState::Failed) {
                error_ = "LSP failure callback failed";
            }
        }
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
