#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "lsp/completion.hpp"
#include "lsp/diagnostics.hpp"
#include "lsp/json.hpp"
#include "lsp/json_rpc.hpp"
#include "lsp/navigation.hpp"
#include "lsp/protocol.hpp"
#include "lsp/refactor.hpp"
#include "lsp/session.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

using namespace cind;

namespace {

class WakeSignal {
public:
    void notify() {
        std::scoped_lock lock(mutex_);
        notified_ = true;
        condition_.notify_one();
    }

    bool wait() {
        std::unique_lock lock(mutex_);
        const bool ready =
            condition_.wait_for(lock, std::chrono::seconds(10), [&] { return notified_; });
        notified_ = false;
        return ready;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool notified_ = false;
};

} // namespace

TEST_CASE("generic LSP session routes requests, errors, cancellation, and server messages") {
    using Json = lsp_json::Json;

    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    LspSession session({1}, runtime,
                       {.command = CIND_LSP_TEST_SERVER,
                        .arguments = {},
                        .root = std::filesystem::temp_directory_path().string(),
                        .language_id = "test",
                        .client_capabilities = {R"({"featureA":{"one":true,"formats":["a"]}})",
                                                R"({"featureA":{"two":true,"formats":["b"]}})"}});
    std::vector<LspNotification> notifications;
    session.set_notification_handler("test/initialized", [&](LspNotification notification) {
        notifications.push_back(std::move(notification));
    });
    session.set_notification_handler("test/serverResponse", [&](LspNotification notification) {
        notifications.push_back(std::move(notification));
    });
    session.set_notification_handler("test/cancelled", [&](LspNotification notification) {
        notifications.push_back(std::move(notification));
    });
    session.set_server_request_handler(
        "test/serverRequest",
        [](LspServerRequest request) -> std::expected<std::string, LspResponseError> {
            if (request.method != "test/serverRequest") {
                return std::unexpected(LspResponseError{
                    .code = -32601, .message = "unsupported test request", .data = "null"});
            }
            const Json params = lsp_json::parse_or_throw(request.params);
            return lsp_json::dump(Json{{"answer", params["question"]}});
        });

    std::optional<LspResponse> echo;
    std::optional<LspResponseError> request_error;
    const auto requested = session.request(
        {.method = "test/echo", .params = R"({"value":42})"},
        [&](LspResponse response) { echo = std::move(response); },
        [&](LspResponseError error) { request_error = std::move(error); });
    REQUIRE(requested.has_value());
    while (!echo && !request_error) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    REQUIRE(echo.has_value());
    CHECK(lsp_json::equal(lsp_json::parse_or_throw(echo->result), Json{{"value", 42}}));
    CHECK(session.capability_boolean({"testProvider", "enabled"}));
    REQUIRE(session.capability({"completionProvider"}).has_value());

    std::optional<LspResponse> client_capabilities;
    const auto queried_capabilities = session.request(
        {.method = "test/clientCapabilities", .params = "null"},
        [&](LspResponse response) { client_capabilities = std::move(response); },
        [](LspResponseError) {});
    REQUIRE(queried_capabilities.has_value());
    while (!client_capabilities) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    const Json advertised = lsp_json::parse_or_throw(client_capabilities->result);
    CHECK(advertised["featureA"]["one"].get<bool>());
    CHECK(advertised["featureA"]["two"].get<bool>());
    CHECK(lsp_json::equal(advertised["featureA"]["formats"], Json::array_t{"a", "b"}));

    const auto has_notification = [&](std::string_view method) {
        return std::ranges::any_of(notifications, [method](const LspNotification& notification) {
            return notification.method == method;
        });
    };
    while (!has_notification("test/initialized") || !has_notification("test/serverResponse")) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    const auto server_response =
        std::ranges::find_if(notifications, [](const LspNotification& notification) {
            return notification.method == "test/serverResponse";
        });
    REQUIRE(server_response != notifications.end());
    CHECK(lsp_json::parse_or_throw(server_response->params)["result"]["answer"]
              .get<std::uint64_t>() == 42);

    std::optional<LspResponseError> controlled_error;
    const auto failed = session.request(
        {.method = "test/fail", .params = "null"}, [](LspResponse) {},
        [&](LspResponseError error) { controlled_error = std::move(error); });
    REQUIRE(failed.has_value());
    while (!controlled_error) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(controlled_error->code == -32042);
    CHECK(controlled_error->message == "controlled failure");
    CHECK_FALSE(lsp_json::parse_or_throw(controlled_error->data)["retry"].get<bool>());

    bool cancelled = false;
    const auto pending = session.request(
        {.method = "test/never", .params = "null"}, [](LspResponse) {}, [](LspResponseError) {},
        [&] { cancelled = true; });
    REQUIRE(pending.has_value());
    (*pending)();
    CHECK(cancelled);
    while (!has_notification("test/cancelled")) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
}

TEST_CASE("generic LSP document synchronization publishes each revision once") {
    using Json = lsp_json::Json;

    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    LspSession session({1}, runtime,
                       {.command = CIND_LSP_TEST_SERVER,
                        .arguments = {},
                        .root = std::filesystem::temp_directory_path().string(),
                        .language_id = "test",
                        .client_capabilities = {}});
    const LspDocumentSnapshot initial{.uri = "file:///tmp/cind-lsp-sync.test",
                                      .language_id = "test",
                                      .revision = 1,
                                      .text = Text("one")};
    REQUIRE(session.synchronize_document(initial).has_value());
    REQUIRE(session.synchronize_document(initial).has_value());
    std::optional<LspResponse> initial_counts;
    const auto initialized = session.request(
        {.method = "test/documentCounts", .params = "null"},
        [&](LspResponse response) { initial_counts = std::move(response); },
        [](LspResponseError) {});
    REQUIRE(initialized.has_value());
    while (!initial_counts) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(lsp_json::equal(lsp_json::parse_or_throw(initial_counts->result),
                          Json{{"opens", 1}, {"changes", 0}, {"closes", 0}}));

    REQUIRE(session.synchronize_document(initial).has_value());
    REQUIRE(session
                .synchronize_document({.uri = initial.uri,
                                       .language_id = initial.language_id,
                                       .revision = 2,
                                       .text = Text("two")})
                .has_value());
    CHECK_FALSE(session
                    .synchronize_document({.uri = initial.uri,
                                           .language_id = initial.language_id,
                                           .revision = 1,
                                           .text = Text("stale")})
                    .has_value());
    REQUIRE(
        session
            .synchronize_document(
                {.uri = initial.uri, .language_id = "other", .revision = 2, .text = Text("two")})
            .has_value());

    std::optional<LspResponse> counts;
    const auto requested = session.request(
        {.method = "test/documentCounts", .params = "null"},
        [&](LspResponse response) { counts = std::move(response); }, [](LspResponseError) {});
    REQUIRE(requested.has_value());
    while (!counts) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(lsp_json::equal(lsp_json::parse_or_throw(counts->result),
                          Json{{"opens", 2}, {"changes", 1}, {"closes", 1}}));
}

TEST_CASE("JSON-RPC framer accepts split and coalesced messages") {
    JsonRpcFramer framer;
    const std::string first = frame_json_rpc(R"({"id":1})");
    const std::string second = frame_json_rpc(R"({"id":2})");
    const std::size_t split = first.size() / 2;

    const auto prefix = framer.push(std::string_view(first).substr(0, split));
    REQUIRE(prefix.has_value());
    CHECK(prefix->empty());
    const auto completed = framer.push(first.substr(split) + second);
    REQUIRE(completed.has_value());
    REQUIRE(completed->size() == 2);
    CHECK((*completed)[0] == R"({"id":1})");
    CHECK((*completed)[1] == R"({"id":2})");
}

TEST_CASE("LSP positions use UTF-16 code units and round trip to text ranges") {
    const Text text("zero\nA😀B\n");
    const TextOffset after_emoji{10};
    CHECK(lsp_position(text, after_emoji).line == 1);
    CHECK(lsp_position(text, after_emoji).character == 3);

    const std::optional<TextRange> range = text_range_from_lsp(
        text, {.start = {.line = 1, .character = 1}, .end = {.line = 1, .character = 3}});
    REQUIRE(range.has_value());
    CHECK(text.substring(*range) == "😀");
}

TEST_CASE("file URIs round trip escaped local paths and reject remote authorities") {
    const std::string path = "/tmp/cind path#value.cpp";
    const std::expected<std::string, std::string> decoded =
        file_uri_to_path(path_to_file_uri(path));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == path);
    CHECK(file_uri_to_path("file://localhost/tmp/local.cpp") == "/tmp/local.cpp");
    CHECK_FALSE(file_uri_to_path("file://remote/tmp/source.cpp").has_value());
    CHECK_FALSE(file_uri_to_path("https://example.com/source.cpp").has_value());
}

TEST_CASE("LSP navigation decodes locations and location links") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    LspSession session({1}, runtime,
                       {.command = CIND_LSP_TEST_SERVER,
                        .arguments = {},
                        .root = std::filesystem::temp_directory_path().string(),
                        .language_id = "cpp",
                        .client_capabilities = {LspDiagnosticsFeature::client_capabilities(),
                                                LspNavigationFeature::client_capabilities()}});
    const LspNavigationRequest request{
        .uri = "file:///tmp/origin.cpp",
        .language_id = "cpp",
        .revision = 1,
        .text = Text("int value;\n"),
        .caret = TextOffset{4},
        .include_declaration = true,
    };

    std::optional<std::vector<LspLocation>> definitions;
    std::optional<LspPublishedDiagnostics> diagnostics;
    std::string error;
    LspDiagnosticsFeature::attach(
        session, [&](LspPublishedDiagnostics published) { diagnostics = std::move(published); },
        [&](std::string message) { error = std::move(message); });
    auto started = LspNavigationFeature::request(
        session, LspNavigationKind::Definition, request,
        [&](std::vector<LspLocation> locations) { definitions = std::move(locations); },
        [&](std::string message) { error = std::move(message); });
    REQUIRE(started.has_value());
    while (!definitions && error.empty()) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    INFO(error);
    CHECK(error.empty());
    REQUIRE(definitions.has_value());
    REQUIRE(definitions->size() == 1);
    CHECK((*definitions)[0].resource == "/tmp/origin.cpp.definition");
    CHECK((*definitions)[0].range.start.line == 0);
    CHECK((*definitions)[0].range.start.character == 3);
    CHECK(LspNavigationFeature::supported(session, LspNavigationKind::Definition));
    REQUIRE(diagnostics.has_value());
    CHECK(diagnostics->uri == request.uri);
    CHECK(diagnostics->version == request.revision);
    REQUIRE(diagnostics->diagnostics.size() == 1);
    CHECK(diagnostics->diagnostics[0].severity == DiagnosticSeverity::Warning);
    CHECK(diagnostics->diagnostics[0].message == "test warning");

    std::optional<std::vector<LspLocation>> references;
    started = LspNavigationFeature::request(
        session, LspNavigationKind::References, request,
        [&](std::vector<LspLocation> locations) { references = std::move(locations); },
        [&](std::string message) { error = std::move(message); });
    REQUIRE(started.has_value());
    while (!references && error.empty()) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(error.empty());
    REQUIRE(references.has_value());
    REQUIRE(references->size() == 2);
    CHECK((*references)[0].resource == "/tmp/first.cpp");
    CHECK((*references)[1].resource == "/tmp/second.cpp");
    CHECK((*references)[1].range.start.character == 1);
}

TEST_CASE("LSP refactors decode workspace edits and resolve code actions") {
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    LspSession session({1}, runtime,
                       {.command = CIND_LSP_TEST_SERVER,
                        .arguments = {},
                        .root = std::filesystem::temp_directory_path().string(),
                        .language_id = "cpp",
                        .client_capabilities = {LspRefactorFeature::client_capabilities()}});
    const LspRefactorRequest request{.uri = "file:///tmp/origin.cpp",
                                     .language_id = "cpp",
                                     .revision = 1,
                                     .text = Text("int value;\n"),
                                     .caret = TextOffset{5},
                                     .range = {TextOffset{4}, TextOffset{9}}};
    std::string error;
    std::optional<LspWorkspaceEdit> renamed;
    auto started = LspRefactorFeature::rename(
        session, request, "renamed",
        [&](std::optional<LspWorkspaceEdit> edit) { renamed = std::move(edit); },
        [&](std::string message) { error = std::move(message); });
    REQUIRE(started.has_value());
    while (!renamed && error.empty()) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(error.empty());
    REQUIRE(renamed.has_value());
    REQUIRE(renamed->documents.size() == 2);
    CHECK(renamed->documents[0].resource == "/tmp/origin.cpp");
    CHECK(renamed->documents[0].version == 1);
    CHECK(renamed->documents[0].edits[0].new_text == "renamed");
    CHECK(LspRefactorFeature::supports_rename(session));

    std::optional<std::vector<LspCodeAction>> actions;
    started = LspRefactorFeature::code_actions(
        session, request, [&](std::vector<LspCodeAction> value) { actions = std::move(value); },
        [&](std::string message) { error = std::move(message); });
    REQUIRE(started.has_value());
    while (!actions && error.empty()) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    INFO(error);
    CHECK(error.empty());
    REQUIRE(actions.has_value());
    REQUIRE(actions->size() == 1);
    CHECK((*actions)[0].title == "Qualify name");
    CHECK_FALSE((*actions)[0].resolved);
    CHECK(LspRefactorFeature::supports_code_actions(session));
    CHECK(LspRefactorFeature::supports_code_action_resolve(session));

    std::optional<LspCodeAction> resolved;
    started = LspRefactorFeature::resolve_code_action(
        session, (*actions)[0].raw, [&](LspCodeAction value) { resolved = std::move(value); },
        [&](std::string message) { error = std::move(message); });
    REQUIRE(started.has_value());
    while (!resolved && error.empty()) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(error.empty());
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->edit.has_value());
    REQUIRE(resolved->edit->documents.size() == 1);
    CHECK(resolved->edit->documents[0].edits[0].new_text == "::");
}

TEST_CASE("clangd session initializes, synchronizes a document, and completes") {
    if (!std::filesystem::exists("/usr/bin/clangd")) {
        return;
    }
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    LspSession session(
        {1}, runtime,
        {.command = "/usr/bin/clangd",
         .arguments = {"--background-index=false", "--clang-tidy=false", "--log=error"},
         .root = std::filesystem::temp_directory_path().string(),
         .language_id = "cpp",
         .client_capabilities = {LspCompletionFeature::client_capabilities()}});
    const std::string source = "struct Foo { int bar; }; int main() { Foo value; value.ba }\n";
    const std::size_t caret_value = source.rfind("ba") + 2;
    std::optional<LspCompletionResponse> response;
    std::string error;
    bool cancelled = false;
    const auto requested = LspCompletionFeature::request(
        session,
        {.uri = path_to_file_uri("/tmp/cind-lsp-completion-test.cpp"),
         .language_id = "cpp",
         .revision = 1,
         .text = Text(source),
         .caret = TextOffset{static_cast<std::uint32_t>(caret_value)},
         .trigger = LspCompletionTriggerKind::Invoked,
         .trigger_character = {}},
        [&](LspCompletionResponse completed) { response = std::move(completed); },
        [&](std::string message) { error = std::move(message); }, [&] { cancelled = true; });
    REQUIRE(requested.has_value());
    while (!response && error.empty() && !cancelled) {
        REQUIRE(wake.wait());
        (void)runtime.drain();
    }
    CHECK(error.empty());
    CHECK_FALSE(cancelled);
    REQUIRE(response.has_value());
    std::string labels;
    for (const LspCompletionItem& item : response->items) {
        labels.append(item.label).append("|");
    }
    INFO(labels);
    CHECK(std::ranges::any_of(response->items, [](const LspCompletionItem& item) {
        return item.label.find("bar") != std::string::npos || item.insert_text == "bar";
    }));
}
