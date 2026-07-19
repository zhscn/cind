#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "lsp/json_rpc.hpp"
#include "lsp/protocol.hpp"
#include "lsp/session.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>

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
        const bool ready = condition_.wait_for(lock, std::chrono::seconds(10),
                                               [&] { return notified_; });
        notified_ = false;
        return ready;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool notified_ = false;
};

} // namespace

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

TEST_CASE("clangd session initializes, synchronizes a document, and completes") {
    if (!std::filesystem::exists("/usr/bin/clangd")) {
        return;
    }
    WakeSignal wake;
    AsyncRuntime runtime([&wake] { wake.notify(); });
    LspSession session({1}, runtime,
                       {.command = "/usr/bin/clangd",
                        .arguments = {"--background-index=false", "--clang-tidy=false", "--log=error"},
                        .root = std::filesystem::temp_directory_path().string(),
                        .language_id = "cpp"});
    const std::string source = "struct Foo { int bar; }; int main() { Foo value; value.ba }\n";
    const std::size_t caret_value = source.rfind("ba") + 2;
    std::optional<LspCompletionResponse> response;
    std::string error;
    bool cancelled = false;
    const auto requested = session.request_completion(
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
