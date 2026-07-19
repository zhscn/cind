#pragma once

#include "async/runtime.hpp"
#include "lsp/json_rpc.hpp"
#include "lsp/protocol.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
    bool completion_resolve = false;
    std::string error;
};

class LspSession {
public:
    using Completed = std::function<void(LspCompletionResponse)>;
    using ResolveCompleted = std::function<void(LspCompletionItem)>;
    using Failed = std::function<void(std::string)>;
    using Cancelled = std::function<void()>;
    using Cancel = std::function<void()>;

    LspSession(LspSessionId id, AsyncRuntime& runtime, LspSessionConfig config);
    ~LspSession();

    LspSession(const LspSession&) = delete;
    LspSession& operator=(const LspSession&) = delete;

    LspSessionId id() const { return id_; }
    const LspSessionConfig& config() const { return config_; }
    LspSessionSnapshot snapshot() const;

    std::expected<Cancel, std::string> request_completion(LspCompletionRequest request,
                                                          Completed completed, Failed failed,
                                                          Cancelled cancelled);
    std::expected<Cancel, std::string> request_completion_resolve(std::string item,
                                                                  ResolveCompleted completed,
                                                                  Failed failed,
                                                                  Cancelled cancelled);
    bool supports_completion_resolve() const { return completion_resolve_; }
    bool cancel_request(std::uint64_t request);
    bool close_document(std::string_view uri);
    void stop() noexcept;

private:
    struct PendingCompletion {
        std::uint64_t id = 0;
        LspCompletionRequest request;
        Completed completed;
        Failed failed;
        Cancelled cancelled;
        bool sent = false;
    };

    struct PendingResolve {
        std::uint64_t id = 0;
        std::string item;
        ResolveCompleted completed;
        Failed failed;
        Cancelled cancelled;
        bool sent = false;
    };

    struct OpenDocument {
        RevisionId revision = 0;
    };

    void start();
    void process_started(AsyncProcessId process);
    void process_output(const std::string& bytes);
    void process_error_output(const std::string& bytes);
    void process_completed(AsyncProcessResult result);
    void process_failed(const std::exception_ptr& failure);
    void handle_message(std::string_view message);
    void initialize_completed(std::string_view message);
    bool send_pending(PendingCompletion& pending);
    bool send_pending(PendingResolve& pending);
    void sync_document(const LspCompletionRequest& request);
    bool send_json(const std::string& json);
    void fail(std::string error);
    void fail_pending(const PendingCompletion& pending, std::string error);
    void fail_pending(const PendingResolve& pending, std::string error);
    static std::string exception_message(const std::exception_ptr& failure);

    LspSessionId id_;
    AsyncRuntime* runtime_;
    LspSessionConfig config_;
    LspSessionState state_ = LspSessionState::Stopped;
    AsyncProcessId process_;
    JsonRpcFramer framer_;
    std::uint64_t next_request_ = 0;
    std::uint64_t initialize_request_ = 0;
    std::unordered_map<std::uint64_t, PendingCompletion> pending_;
    std::unordered_map<std::uint64_t, PendingResolve> pending_resolves_;
    std::unordered_map<std::string, OpenDocument> documents_;
    std::string standard_error_;
    std::string error_;
    bool stopping_ = false;
    bool completion_resolve_ = false;
};

} // namespace cind
