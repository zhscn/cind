#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "syntax/syntax_tree.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace cind;

TEST_CASE("immutable token and syntax queries support concurrent readers") {
    std::string source;
    for (int index = 0; index < 200; ++index) {
        source += "namespace n" + std::to_string(index) + " { int f() { return " +
                  std::to_string(index) + "; } }\n";
    }
    const SyntaxTree tree = parse(source);
    const TokenBuffer& tokens = tree.tokens();
    REQUIRE(tokens.size() > 1000);

    std::atomic_bool valid = true;
    std::vector<std::jthread> readers;
    for (int reader = 0; reader < 8; ++reader) {
        readers.emplace_back([&, reader] {
            for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
                const std::size_t token_index =
                    (iteration * 97 + static_cast<std::size_t>(reader)) % tokens.size();
                const Token token = tokens[token_index];
                const SyntaxNodeId id = tree.node_at(token.range.start);
                const SyntaxNode& node = tree.node(id);
                const TextRange range = tree.node_range(id);
                if (tree.node(tree.root()).kind != SyntaxKind::TranslationUnit ||
                    range.start > token.range.start || node.first_token > node.end_token) {
                    valid.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    readers.clear();
    CHECK(valid.load(std::memory_order_relaxed));
}
