#include "lsp/json.hpp"
#include "lsp/json_rpc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

using Json = cind::lsp_json::Json;

void send(const Json& message) {
    std::cout << cind::frame_json_rpc(cind::lsp_json::dump(message)) << std::flush;
}

Json position(std::uint64_t line, std::uint64_t character) {
    return Json::object_t{{"line", line}, {"character", character}};
}

Json range(std::uint64_t start_line, std::uint64_t start_character, std::uint64_t end_line,
           std::uint64_t end_character) {
    return Json::object_t{{"start", position(start_line, start_character)},
                          {"end", position(end_line, end_character)}};
}

void publish_diagnostic(std::string uri, std::uint64_t version) {
    const Json diagnostic = Json::object_t{{"range", range(0, 1, 0, 3)},
                                           {"severity", 2},
                                           {"source", "cind-test"},
                                           {"code", "W1"},
                                           {"message", "test warning"}};
    send(Json::object_t{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", Json::object_t{{"uri", std::move(uri)},
                                  {"version", version},
                                  {"diagnostics", Json::array_t{diagnostic}}}},
    });
}

} // namespace

int main() {
    cind::JsonRpcFramer framer;
    std::array<char, 4096> bytes{};
    std::size_t opens = 0;
    std::size_t changes = 0;
    std::size_t closes = 0;
    Json client_capabilities = Json::object_t{};
    while (true) {
        const ssize_t count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
        if (count <= 0) {
            return 0;
        }
        const auto messages =
            framer.push(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
        if (!messages) {
            return 2;
        }
        for (const std::string& payload : *messages) {
            const Json message = cind::lsp_json::parse_or_throw(payload);
            const std::string method = cind::lsp_json::value_or(message, "method", std::string{});
            if (method == "initialize") {
                client_capabilities = message["params"]["capabilities"];
                const Json capabilities{
                    {"completionProvider", {{"resolveProvider", true}}},
                    {"definitionProvider", true},
                    {"declarationProvider", true},
                    {"implementationProvider", true},
                    {"referencesProvider", true},
                    {"renameProvider", true},
                    {"codeActionProvider", {{"resolveProvider", true}}},
                    {"testProvider", {{"enabled", true}}},
                };
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"result", {{"capabilities", capabilities}}}});
            } else if (method == "initialized") {
                send({{"jsonrpc", "2.0"},
                      {"method", "test/initialized"},
                      {"params", {{"ready", true}}}});
                send({{"jsonrpc", "2.0"},
                      {"id", "server-request"},
                      {"method", "test/serverRequest"},
                      {"params", {{"question", 42}}}});
            } else if (method == "test/echo") {
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"result", message.contains("params") ? message["params"] : Json(nullptr)}});
            } else if (method == "test/fail") {
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"error",
                       {{"code", -32042},
                        {"message", "controlled failure"},
                        {"data", {{"retry", false}}}}}});
            } else if (method == "$/cancelRequest") {
                send({{"jsonrpc", "2.0"},
                      {"method", "test/cancelled"},
                      {"params", message["params"]}});
            } else if (method == "textDocument/didOpen") {
                ++opens;
                publish_diagnostic(
                    message["params"]["textDocument"]["uri"].get<std::string>(),
                    message["params"]["textDocument"]["version"].get<std::uint64_t>());
            } else if (method == "textDocument/didChange") {
                ++changes;
                publish_diagnostic(
                    message["params"]["textDocument"]["uri"].get<std::string>(),
                    message["params"]["textDocument"]["version"].get<std::uint64_t>());
            } else if (method == "textDocument/didClose") {
                ++closes;
            } else if (method == "test/documentCounts") {
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"result", {{"opens", opens}, {"changes", changes}, {"closes", closes}}}});
            } else if (method == "test/clientCapabilities") {
                send({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", client_capabilities}});
            } else if (method == "textDocument/definition") {
                const std::string target_uri =
                    message["params"]["textDocument"]["uri"].get<std::string>() + ".definition";
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"result", Json::object_t{{"targetUri", target_uri},
                                                {"targetRange", range(0, 0, 0, 5)},
                                                {"targetSelectionRange", range(0, 3, 0, 5)}}}});
            } else if (method == "textDocument/references") {
                const Json first =
                    Json::object_t{{"uri", "file:///tmp/first.cpp"}, {"range", range(1, 3, 1, 6)}};
                const Json second = Json::object_t{{"uri", "file://localhost/tmp/second.cpp"},
                                                   {"range", range(4, 1, 4, 2)}};
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"result", Json::array_t{first, second}}});
            } else if (method == "textDocument/rename") {
                const std::string uri = message["params"]["textDocument"]["uri"].get<std::string>();
                const std::string name = message["params"]["newName"].get<std::string>();
                const Json first_edit =
                    Json::object_t{{"range", range(0, 4, 0, 9)}, {"newText", name}};
                const Json second_edit =
                    Json::object_t{{"range", range(1, 0, 1, 3)}, {"newText", name}};
                const Json documents = Json::array_t{
                    Json::object_t{
                        {"textDocument", Json::object_t{{"uri", uri}, {"version", 1}}},
                        {"edits", Json::array_t{first_edit}},
                    },
                    Json::object_t{
                        {"textDocument", Json::object_t{{"uri", "file:///tmp/rename-other.cpp"},
                                                        {"version", nullptr}}},
                        {"edits", Json::array_t{second_edit}},
                    },
                };
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"result", {{"documentChanges", documents}}}});
            } else if (method == "textDocument/codeAction") {
                const Json action = Json::object_t{
                    {"title", "Qualify name"},
                    {"kind", "quickfix"},
                    {"isPreferred", true},
                    {"data", Json::object_t{{"fix", 1}}},
                };
                send(
                    {{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", Json::array_t{action}}});
            } else if (method == "codeAction/resolve") {
                Json action = message["params"];
                const Json insertion =
                    Json::object_t{{"range", range(0, 0, 0, 0)}, {"newText", "::"}};
                action["edit"] = Json::object_t{
                    {"changes",
                     Json::object_t{
                         {"file:///tmp/origin.cpp", Json::array_t{insertion}},
                     }},
                };
                send({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", action}});
            } else if (method.empty() && message.contains("id") && message["id"].is_string() &&
                       message["id"].get<std::string>() == "server-request") {
                send({{"jsonrpc", "2.0"}, {"method", "test/serverResponse"}, {"params", message}});
            }
        }
    }
}
