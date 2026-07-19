#pragma once

#include "async/runtime.hpp"
#include "lsp/json_rpc.hpp"
#include "lsp/request.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cind {

struct LspSessionId {
    std::uint64_t value = 0;

    bool valid() const { return value != 0; }
    friend bool operator==(LspSessionId, LspSessionId) = default;
};

struct LspSessionConfig {
    std::string command;
    std::vector<std::string> arguments;
    std::string root;
    std::string language_id;
    std::vector<std::string> client_capabilities;
};

enum class LspSessionState : std::uint8_t {
    Stopped,
    Starting,
    Initializing,
    Ready,
    Failed,
};

struct LspSessionSnapshot {
    LspSessionId id;
    LspSessionState state = LspSessionState::Stopped;
    std::string command;
    std::string root;
    std::size_t pending_requests = 0;
    std::size_t open_documents = 0;
    std::string server_capabilities = "{}";
    std::string error;
};

class LspSession {
public:
    using Completed = std::function<void(LspResponse)>;
    using Failed = std::function<void(LspResponseError)>;
    using Cancelled = std::function<void()>;
    using Cancel = std::function<void()>;
    using NotificationHandler = std::function<void(LspNotification)>;
    using ServerRequestHandler =
        std::function<std::expected<std::string, LspResponseError>(LspServerRequest)>;

    LspSession(LspSessionId id, AsyncRuntime& runtime, LspSessionConfig config);
    ~LspSession();

    LspSession(const LspSession&) = delete;
    LspSession& operator=(const LspSession&) = delete;

    LspSessionId id() const { return id_; }
    const LspSessionConfig& config() const { return config_; }
    LspSessionSnapshot snapshot() const;

    std::expected<Cancel, std::string> request(LspRequest request, Completed completed,
                                               Failed failed, Cancelled cancelled = {});
    std::expected<void, std::string> notify(LspNotification notification);
    std::expected<void, std::string> synchronize_document(LspDocumentSnapshot document);
    std::optional<std::string> capability(std::initializer_list<std::string_view> path) const;
    bool capability_boolean(std::initializer_list<std::string_view> path,
                            bool fallback = false) const;
    void set_notification_handler(std::string method, NotificationHandler handler);
    bool clear_notification_handler(std::string_view method);
    void set_server_request_handler(std::string method, ServerRequestHandler handler);
    bool clear_server_request_handler(std::string_view method);
    bool cancel_request(std::uint64_t request);
    bool close_document(std::string_view uri);
    void stop() noexcept;

private:
    struct PendingRequest {
        std::uint64_t id = 0;
        LspRequest request;
        Completed completed;
        Failed failed;
        Cancelled cancelled;
        bool sent = false;
    };

    struct OpenDocument {
        std::string language_id;
        RevisionId revision = 0;
        Text text;
        bool published = false;
    };

    void start();
    void process_started(AsyncProcessId process);
    void process_output(const std::string& bytes);
    void process_error_output(const std::string& bytes);
    void process_completed(AsyncProcessResult result);
    void process_failed(const std::exception_ptr& failure);
    void handle_message(std::string_view message);
    void initialize_completed(std::string_view message);
    void handle_server_request(std::string_view id, std::string_view method,
                               std::string_view params);
    void handle_notification(std::string_view method, std::string_view params);
    bool send_pending(PendingRequest& pending);
    bool publish_document(std::string_view uri, OpenDocument& document);
    bool flush_documents();
    bool send_json(const std::string& json);
    void fail(std::string error);
    void fail_pending(const PendingRequest& pending, LspResponseError error);
    static std::string exception_message(const std::exception_ptr& failure);

    LspSessionId id_;
    AsyncRuntime* runtime_;
    LspSessionConfig config_;
    std::string client_capabilities_ = "{}";
    LspSessionState state_ = LspSessionState::Stopped;
    AsyncProcessId process_;
    JsonRpcFramer framer_;
    std::uint64_t next_request_ = 0;
    std::uint64_t initialize_request_ = 0;
    std::unordered_map<std::uint64_t, PendingRequest> pending_;
    std::unordered_map<std::string, OpenDocument> documents_;
    std::string server_capabilities_ = "{}";
    std::unordered_map<std::string, NotificationHandler> notification_handlers_;
    std::unordered_map<std::string, ServerRequestHandler> server_request_handlers_;
    std::string standard_error_;
    std::string error_;
    bool stopping_ = false;
};

} // namespace cind
