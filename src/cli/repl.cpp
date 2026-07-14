#include "cli/repl.hpp"

#include "cli/session.hpp"
#include "cli/style_loader.hpp"

#include <charconv>
#include <iostream>
#include <unistd.h>

namespace cind {

namespace {

void show(const EditSession& session) {
    std::string rendered = session.render_with_caret();
    if (!rendered.ends_with('\n')) {
        rendered += '\n';
    }
    std::cout << "-- rev " << session.snapshot().revision() << " caret "
              << session.caret().value << " --\n"
              << rendered;
}

void print_decision(const IndentDecision& decision) {
    std::cout << "target-column: " << decision.target_column
              << "  role: " << format_role_name(decision.role) << "\n";
    for (const auto& step : decision.trace) {
        std::cout << "  " << step << "\n";
    }
}

constexpr std::string_view kHelp = R"(commands:
  type <text>        insert text at the caret
  enter              press Enter (handler pipeline)
  indent             reindent the caret's line
  caret <offset>     move the caret to a byte offset
  undo / redo
  show               print the document with the caret
  explain            explain the indent of the caret's line
  tree               dump the syntax tree
  style <key> <val>  e.g. style namespace_indentation Inner
  loadstyle <path>   read a .clang-format file (or discover upward from a dir)
  quit
)";

} // namespace

int run_repl(std::istream& in, std::string initial) {
    CaretText split = split_caret_marker(initial);
    EditSession session(std::move(split.text), CppIndentStyle{});
    session.set_caret(TextOffset{
        std::min(split.caret.value, session.snapshot().size_bytes())});

    const bool interactive = isatty(fileno(stdin)) != 0;
    if (interactive) {
        std::cout << kHelp;
        show(session);
    }

    std::string line;
    while (true) {
        if (interactive) {
            std::cout << "> " << std::flush;
        }
        if (!std::getline(in, line)) {
            break;
        }
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        try {
            if (line == "quit" || line == "exit") {
                break;
            } else if (line == "show") {
                show(session);
            } else if (line == "help") {
                std::cout << kHelp;
            } else if (line.starts_with("type ")) {
                session.type_text(std::string_view(line).substr(5));
                show(session);
            } else if (line == "enter") {
                EnterResult result = session.enter();
                std::cout << "handler: " << result.handler
                          << "  role: " << format_role_name(result.decision.role) << "\n";
                show(session);
            } else if (line == "indent") {
                print_decision(session.indent());
                show(session);
            } else if (line == "undo") {
                std::cout << (session.undo() ? "" : "nothing to undo\n");
                show(session);
            } else if (line == "redo") {
                std::cout << (session.redo() ? "" : "nothing to redo\n");
                show(session);
            } else if (line.starts_with("caret ")) {
                std::string_view arg = std::string_view(line).substr(6);
                std::uint32_t offset = 0;
                auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), offset);
                if (ec != std::errc() || ptr != arg.data() + arg.size()) {
                    std::cout << "error: bad offset\n";
                } else {
                    session.set_caret(TextOffset{offset});
                    show(session);
                }
            } else if (line == "explain") {
                print_decision(session.explain());
            } else if (line == "tree") {
                auto snap = session.snapshot();
                std::cout << parse(snap.text()).dump(snap.text());
            } else if (line.starts_with("loadstyle ")) {
                std::string_view path = std::string_view(line).substr(10);
                if (auto loaded = load_clang_format_style(std::string(path))) {
                    session.style() = loaded->style;
                    std::cout << "loaded " << loaded->config_path.string() << "\n";
                    if (loaded->disable_format) {
                        std::cout << "note: DisableFormat: true (clang-format "
                                     "would not touch these files)\n";
                    }
                    for (const std::string& warning : loaded->warnings) {
                        std::cout << "warning: " << warning << "\n";
                    }
                } else {
                    std::cout << "error: no .clang-format found from " << path << "\n";
                }
            } else if (line.starts_with("style ")) {
                std::string_view rest = std::string_view(line).substr(6);
                std::size_t space = rest.find(' ');
                if (space == std::string_view::npos ||
                    !set_style_field(session.style(), rest.substr(0, space),
                                     rest.substr(space + 1))) {
                    std::cout << "error: bad style key or value\n";
                }
            } else {
                std::cout << "error: unknown command (try 'help')\n";
            }
        } catch (const std::exception& e) {
            std::cout << "error: " << e.what() << "\n";
        }
    }
    return 0;
}

} // namespace cind
