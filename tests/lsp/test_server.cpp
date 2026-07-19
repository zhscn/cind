#include "lsp/json_rpc.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

using Json = nlohmann::json;

void send(const Json& message) {
    std::cout << cind::frame_json_rpc(message.dump()) << std::flush;
}

void publish_diagnostic(std::string uri, std::uint64_t version) {
    send({{"jsonrpc", "2.0"},
          {"method", "textDocument/publishDiagnostics"},
          {"params",
           {{"uri", std::move(uri)},
            {"version", version},
            {"diagnostics", Json::array({{{"range",
                                           {{"start", {{"line", 0}, {"character", 1}}},
                                            {"end", {{"line", 0}, {"character", 3}}}}},
                                          {"severity", 2},
                                          {"source", "cind-test"},
                                          {"code", "W1"},
                                          {"message", "test warning"}}})}}}});
}

} // namespace

int main() {
    cind::JsonRpcFramer framer;
    std::array<char, 4096> bytes{};
    std::size_t opens = 0;
    std::size_t changes = 0;
    std::size_t closes = 0;
    Json client_capabilities = Json::object();
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
            const Json message = Json::parse(payload);
            const std::string method = message.value("method", std::string{});
            if (method == "initialize") {
                client_capabilities = message["params"]["capabilities"];
                const Json capabilities{
                    {"completionProvider", {{"resolveProvider", true}}},
                    {"definitionProvider", true},
                    {"declarationProvider", true},
                    {"implementationProvider", true},
                    {"referencesProvider", true},
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
                      {"result", message.value("params", Json(nullptr))}});
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
                      {"result",
                       {{"targetUri", target_uri},
                        {"targetRange",
                         {{"start", {{"line", 0}, {"character", 0}}},
                          {"end", {{"line", 0}, {"character", 5}}}}},
                        {"targetSelectionRange",
                         {{"start", {{"line", 0}, {"character", 3}}},
                          {"end", {{"line", 0}, {"character", 5}}}}}}}});
            } else if (method == "textDocument/references") {
                send({{"jsonrpc", "2.0"},
                      {"id", message["id"]},
                      {"result", Json::array({{{"uri", "file:///tmp/first.cpp"},
                                               {"range",
                                                {{"start", {{"line", 1}, {"character", 3}}},
                                                 {"end", {{"line", 1}, {"character", 6}}}}}},
                                              {{"uri", "file://localhost/tmp/second.cpp"},
                                               {"range",
                                                {{"start", {{"line", 4}, {"character", 1}}},
                                                 {"end", {{"line", 4}, {"character", 2}}}}}}})}});
            } else if (method.empty() && message.value("id", Json()) == "server-request") {
                send({{"jsonrpc", "2.0"}, {"method", "test/serverResponse"}, {"params", message}});
            }
        }
    }
}
