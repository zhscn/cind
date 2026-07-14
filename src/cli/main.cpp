#include "cli/bench.hpp"
#include "cli/fixture_runner.hpp"
#include "cli/repl.hpp"
#include "commands/editor_commands.hpp"
#include "cpp_lexer/lexer.hpp"
#include "document/document.hpp"
#include "syntax/syntax_tree.hpp"

#include <cstdlib>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string escape_preview(std::string_view text, std::size_t max_len = 40) {
    std::string out;
    for (char c : text) {
        if (out.size() >= max_len) {
            out += "...";
            break;
        }
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

int cmd_tokens(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "indent-core: cannot open " << path << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    cind::Document document(buffer.str());
    auto snapshot = document.snapshot();
    auto output = cind::lex(snapshot.text());

    for (const auto& token : output.tokens) {
        auto pos = snapshot.lines().position(token.range.start);
        std::string flags;
        if (has_flag(token.flags, cind::LexicalFlags::PreprocessorLine)) {
            flags += " pp";
        }
        if (has_flag(token.flags, cind::LexicalFlags::Unterminated)) {
            flags += " unterminated";
        }
        if (has_flag(token.flags, cind::LexicalFlags::EscapedNewline)) {
            flags += " splice";
        }
        std::printf("%5u..%-5u %3u:%-3u %-18s |%s|%s\n", token.range.start.value,
                    token.range.end.value, pos.line, pos.byte_column,
                    std::string(cind::token_kind_name(token.kind)).c_str(),
                    escape_preview(snapshot.text(token.range)).c_str(), flags.c_str());
    }
    return 0;
}

int cmd_explain(const char* path, std::uint32_t line_1based) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "indent-core: cannot open " << path << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    cind::Document document(buffer.str());
    auto snapshot = document.snapshot();
    if (line_1based == 0 || line_1based > snapshot.lines().line_count()) {
        std::cerr << "indent-core: line out of range\n";
        return 1;
    }
    auto tree = cind::parse(snapshot.text());
    cind::CppIndentStyle style;
    auto decision = cind::compute_line_indent(snapshot, tree, line_1based - 1, style);

    std::cout << "target-column: " << decision.target_column << "\n";
    std::cout << "role: " << cind::format_role_name(decision.role) << "\n";
    if (decision.preserve) {
        std::cout << "preserve: line must not be reindented\n";
    }
    if (decision.anchor) {
        auto pos = snapshot.lines().position(*decision.anchor);
        std::cout << "anchor: line " << pos.line + 1 << ", column " << pos.byte_column << "\n";
    }
    std::cout << "\ntrace:\n";
    for (const auto& step : decision.trace) {
        std::cout << "  " << step << "\n";
    }
    return 0;
}

int cmd_apply_enter(const char* path, std::uint32_t offset) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "indent-core: cannot open " << path << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    cind::Document document(buffer.str());
    if (offset > document.snapshot().size_bytes()) {
        std::cerr << "indent-core: offset out of range\n";
        return 1;
    }
    cind::CppIndentStyle style;
    auto result = cind::press_enter(document, cind::TextOffset{offset}, style);
    std::string out(document.snapshot().text());
    out.insert(result.caret.value, "^");
    std::cout << "handler: " << result.handler << "\n---\n" << out;
    return 0;
}

int cmd_tree(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "indent-core: cannot open " << path << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    cind::Document document(buffer.str());
    auto snapshot = document.snapshot();
    auto tree = cind::parse(snapshot.text());
    std::cout << tree.dump(snapshot.text());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view command = argc >= 2 ? argv[1] : "";
    if (argc >= 3 && command == "tokens") {
        return cmd_tokens(argv[2]);
    }
    if (argc >= 3 && command == "tree") {
        return cmd_tree(argv[2]);
    }
    if (argc >= 5 && command == "explain" && std::string_view(argv[3]) == "--line") {
        return cmd_explain(argv[2], static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)));
    }
    if (argc >= 5 && command == "apply-enter" && std::string_view(argv[3]) == "--offset") {
        return cmd_apply_enter(argv[2],
                               static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10)));
    }
    if (command == "repl") {
        std::string initial;
        if (argc >= 3) {
            std::ifstream in(argv[2], std::ios::binary);
            if (!in) {
                std::cerr << "indent-core: cannot open " << argv[2] << "\n";
                return 1;
            }
            std::stringstream buffer;
            buffer << in.rdbuf();
            initial = buffer.str();
        }
        return cind::run_repl(std::cin, std::move(initial));
    }
    if (argc >= 3 && command == "test") {
        return cind::run_fixtures(argv[2]);
    }
    if (argc >= 3 && command == "bench") {
        cind::BenchOptions options;
        std::vector<std::string> paths;
        for (int i = 2; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--style" && i + 1 < argc) {
                options.style_preset = argv[++i];
            } else if (arg == "--show" && i + 1 < argc) {
                options.show_mismatches =
                    static_cast<int>(std::strtol(argv[++i], nullptr, 10));
            } else if (arg == "--clean-only") {
                options.clean_only = true;
            } else {
                paths.emplace_back(arg);
            }
        }
        if (!paths.empty()) {
            return cind::run_bench(paths, options);
        }
    }
    std::cerr << "usage: indent-core tokens|tree <file>\n"
                 "       indent-core explain <file> --line <1-based>\n"
                 "       indent-core apply-enter <file> --offset <byte>\n"
                 "       indent-core repl [file]\n"
                 "       indent-core test <fixture.yaml|dir>\n"
                 "       indent-core bench <file|dir>... [--style default|llvm] [--show N] "
                 "[--clean-only]\n";
    return 2;
}
