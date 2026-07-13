#include "cpp_lexer/lexer.hpp"
#include "document/document.hpp"

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

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string_view(argv[1]) == "tokens") {
        return cmd_tokens(argv[2]);
    }
    std::cerr << "usage: indent-core tokens <file>\n";
    return 2;
}
