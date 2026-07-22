#include "cli/bench.hpp"
#include "cli/fixture_runner.hpp"
#include "cli/repl.hpp"
#include "commands/editor_commands.hpp"
#include "cpp_lexer/lexer.hpp"
#include "document/document.hpp"
#include "editor/editor_application.hpp"
#include "script/guile_call_stats.hpp"
#include "syntax/analysis.hpp"
#include "syntax/syntax_tree.hpp"
#include "tui/editor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
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
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
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
    auto output = cind::lex(snapshot.content());

    for (const auto& token : output.tokens) {
        auto pos = snapshot.content().position(token.range.start);
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
                    escape_preview(snapshot.substring(token.range)).c_str(), flags.c_str());
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
    if (line_1based == 0 || line_1based > snapshot.content().line_count()) {
        std::cerr << "indent-core: line out of range\n";
        return 1;
    }
    auto tree = cind::parse(snapshot.content());
    cind::CppIndentStyle style;
    auto decision = cind::compute_line_indent(snapshot, tree, line_1based - 1, style);

    std::cout << "target-column: " << decision.target_column << "\n";
    std::cout << "role: " << cind::format_role_name(decision.role) << "\n";
    if (decision.preserve) {
        std::cout << "preserve: line must not be reindented\n";
    }
    if (decision.anchor) {
        auto pos = snapshot.content().position(*decision.anchor);
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
    std::string out = document.snapshot().content().to_string();
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
    const std::string text = snapshot.content().to_string();
    auto tree = cind::parse(text);
    std::cout << tree.dump(text);
    return 0;
}

// Times each pipeline stage on a real file. The "enter" row is the §6 budget
// metric: one full keystroke (lex + parse + indent + commit) on a fresh
// document. Values are medians of `reps` runs.
int cmd_perf(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "indent-core: cannot open " << path << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    constexpr int reps = 7;
    auto median = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    auto time_ms = [&](auto&& fn) {
        std::vector<double> runs;
        for (int i = 0; i < reps; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            const auto t1 = std::chrono::steady_clock::now();
            runs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return median(runs);
    };

    cind::Document document(text);
    auto snapshot = document.snapshot();
    const std::uint32_t lines = snapshot.content().line_count();
    const cind::TextOffset mid = snapshot.content().line_start(lines / 2);

    std::size_t token_count = 0;
    std::size_t node_count = 0;
    const double build_ms = time_ms([&] { cind::Document d(text); });
    const double lex_ms =
        time_ms([&] { token_count = cind::lex(snapshot.content()).tokens.size(); });
    const double parse_ms =
        time_ms([&] { node_count = cind::parse(snapshot.content()).node_count(); });
    cind::SyntaxTree tree = cind::parse(snapshot.content());
    cind::CppIndentStyle style;
    const double indent_ms =
        time_ms([&] { cind::compute_line_indent(snapshot, tree, lines / 2, style); });
    // Editor steady state: the analyzer is warm; one keystroke costs the
    // incremental relex + reparse + commit.
    std::vector<std::unique_ptr<cind::Document>> docs;
    std::vector<std::unique_ptr<cind::Analyzer>> analyzers;
    for (int i = 0; i < reps; ++i) {
        docs.push_back(std::make_unique<cind::Document>(text));
        analyzers.push_back(std::make_unique<cind::Analyzer>());
        analyzers.back()->analyze(docs.back()->snapshot());
    }
    std::size_t rep = 0;
    const double enter_ms = time_ms([&] {
        const std::size_t i = rep++;
        cind::press_enter(*docs[i], mid, style, *analyzers[i]);
    });
    // Plain character: the common keystroke — one in-place incremental
    // advance, no speculative analysis.
    rep = 0;
    const double char_ms = time_ms([&] {
        const std::size_t i = rep++;
        cind::type_char(*docs[i], mid, 'x', style, *analyzers[i]);
    });
    const double cold_enter_ms = time_ms([&] {
        cind::Document d(text);
        cind::press_enter(d, mid, style);
    });

    std::printf("file:    %s\n", path);
    std::printf("size:    %zu bytes, %u lines, %zu tokens, %zu nodes\n", text.size(), lines,
                token_count, node_count);
    std::printf("build:   %8.3f ms\n", build_ms);
    std::printf("lex:     %8.3f ms\n", lex_ms);
    std::printf("parse:   %8.3f ms   (lex included)\n", parse_ms);
    std::printf("indent:  %8.3f ms   (single line query)\n", indent_ms);
    std::printf("enter:   %8.3f ms   (keystroke, warm analyzer: incremental path)\n", enter_ms);
    std::printf("char:    %8.3f ms   (plain character, in-place incremental)\n", char_ms);
    std::printf("enter0:  %8.3f ms   (keystroke, cold: full lex+parse twice)\n", cold_enter_ms);
    return 0;
}

// Measures the whole application keystroke path, including the C++ -> Guile
// entries it already performs, and reports what share of a keystroke is spent
// inside Scheme. This is the gate for design/09-guile-first.md §7: state
// ownership can only move to Guile if the path retains its latency budget.
int cmd_guile_perf(const char* path, int repetitions) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "indent-core: cannot open " << path << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();

    cind::guile_stats::timing_enabled.store(true, std::memory_order_relaxed);
    cind::EditorApplication application({.path = path,
                                         .initial_text = std::move(text),
                                         .initial_line = 0,
                                         .platform_services = {},
                                         .init_file = std::nullopt});

    const std::uint32_t lines = application.session().snapshot().content().line_count();

    const cind::KeyStroke key = cind::KeyStroke::character_key(U'x');
    auto keystroke = [&] {
        if (!application.handle_key(key, 40)) {
            application.insert_text("x");
        }
    };
    // Typing forever at one spot degenerates into a tiny damage window in one
    // identifier. Roam the caret so edits land in varied syntactic contexts and
    // the incremental paths pay their real cost.
    std::uint32_t roam = 0;
    auto move_caret = [&] {
        roam = (roam * 1103515245u + 12345u) % lines;
        const cind::DocumentSnapshot snapshot = application.session().snapshot();
        application.session().set_caret(snapshot.content().line_start(roam));
    };
    constexpr int roam_interval = 8;

    constexpr int warmup = 200;
    for (int index = 0; index < warmup; ++index) {
        if (index % roam_interval == 0) {
            move_caret();
        }
        keystroke();
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repetitions));
    const cind::guile_stats::Sample before = cind::guile_stats::sample();
    const auto measured_start = std::chrono::steady_clock::now();
    // Per-keystroke Guile time attributes the latency tail: a slow keystroke is
    // either a Scheme/GC pause or kernel work (full reparse), and the two ask
    // for different fixes.
    std::vector<double> guile_samples;
    guile_samples.reserve(static_cast<std::size_t>(repetitions));
    for (int index = 0; index < repetitions; ++index) {
        if (index % roam_interval == 0) {
            move_caret();
        }
        const cind::guile_stats::Sample key_before = cind::guile_stats::sample();
        const auto t0 = std::chrono::steady_clock::now();
        keystroke();
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        guile_samples.push_back(
            static_cast<double>(cind::guile_stats::since(key_before).nanoseconds) / 1e6);
    }
    const double total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - measured_start)
            .count();
    const cind::guile_stats::Sample guile = cind::guile_stats::since(before);

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    auto quantile = [&](double fraction) {
        const std::size_t index =
            static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
        return sorted[index];
    };

    const double guile_total_ms = static_cast<double>(guile.nanoseconds) / 1e6;
    const double calls_per_key = static_cast<double>(guile.calls) / repetitions;

    std::printf("file:            %s\n", path);
    std::printf("lines:           %u\n", lines);
    std::printf("keystrokes:      %d (after %d warmup)\n", repetitions, warmup);
    std::printf("\n-- keystroke latency (full application path) --\n");
    std::printf("median:          %8.3f ms\n", quantile(0.50));
    std::printf("p90:             %8.3f ms\n", quantile(0.90));
    std::printf("p99:             %8.3f ms\n", quantile(0.99));
    std::printf("max:             %8.3f ms   (includes GC pauses)\n", sorted.back());
    std::printf("\n-- Guile share (already on today's key path) --\n");
    std::printf("calls/keystroke: %8.2f\n", calls_per_key);
    std::printf("guile/keystroke: %8.3f ms\n", guile_total_ms / repetitions);
    std::printf("guile share:     %8.1f %%\n", 100.0 * guile_total_ms / total_ms);
    std::printf("per call:        %8.3f us\n",
                guile.calls > 0 ? (guile_total_ms * 1000.0) / static_cast<double>(guile.calls)
                                : 0.0);

    std::vector<std::size_t> order(samples.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(),
              [&](std::size_t left, std::size_t right) { return samples[left] > samples[right]; });
    std::printf("\n-- latency tail attribution (slowest 5 keystrokes) --\n");
    for (std::size_t rank = 0; rank < 5 && rank < order.size(); ++rank) {
        const std::size_t index = order[rank];
        const double total = samples[index];
        const double in_guile = guile_samples[index];
        std::printf("  #%-6zu %8.3f ms  guile %7.3f ms (%5.1f %%)  %s\n", index, total, in_guile,
                    total > 0.0 ? 100.0 * in_guile / total : 0.0,
                    in_guile > 0.5 * total ? "scheme/GC" : "kernel");
    }
    // A periodic tail points at recurring work on the key path; a scattered one
    // points at allocation-driven collection.
    std::size_t over_1ms = 0;
    std::size_t over_5ms = 0;
    for (const double value : samples) {
        over_1ms += value > 1.0 ? 1 : 0;
        over_5ms += value > 5.0 ? 1 : 0;
    }
    std::printf("keystrokes > 1ms: %zu / %zu     > 5ms: %zu\n", over_1ms, samples.size(), over_5ms);
    if (over_5ms > 1) {
        std::vector<std::size_t> spikes;
        for (std::size_t index = 0; index < samples.size(); ++index) {
            if (samples[index] > 5.0) {
                spikes.push_back(index);
            }
        }
        std::printf("spike gaps:      ");
        for (std::size_t index = 1; index < spikes.size() && index < 12; ++index) {
            std::printf("%zu ", spikes[index] - spikes[index - 1]);
        }
        std::printf("\n");
    }
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
    if (argc >= 3 && command == "edit") {
        std::uint32_t initial_line = 0;
        int file_arg = 2;
        if (argv[2][0] == '+' && argc >= 4) {
            initial_line = static_cast<std::uint32_t>(std::strtoul(argv[2] + 1, nullptr, 10));
            file_arg = 3;
        }
        return cind::tui::run_editor(argv[file_arg], initial_line);
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
    if (argc >= 3 && command == "perf") {
        return cmd_perf(argv[2]);
    }
    if (argc >= 3 && command == "guile-perf") {
        const int repetitions =
            argc >= 4 ? static_cast<int>(std::strtol(argv[3], nullptr, 10)) : 2000;
        return cmd_guile_perf(argv[2], repetitions);
    }
    if (argc >= 3 && command == "bench") {
        cind::BenchOptions options;
        std::vector<std::string> paths;
        for (int i = 2; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--style" && i + 1 < argc) {
                options.style_preset = argv[++i];
            } else if (arg == "--show" && i + 1 < argc) {
                options.show_mismatches = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
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
                 "       indent-core edit [+LINE] <file>\n"
                 "       indent-core repl [file]\n"
                 "       indent-core test <fixture.yaml|dir>\n"
                 "       indent-core bench <file|dir>... [--style default|file|<preset>] "
                 "[--show N] [--clean-only]\n";
    return 2;
}
