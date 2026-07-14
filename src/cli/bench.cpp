#include "cli/bench.hpp"

#include "document/document.hpp"
#include "indentation/indentation_service.hpp"
#include "syntax/syntax_tree.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

namespace cind {

namespace {

namespace fs = std::filesystem;

bool has_cpp_extension(const fs::path& path) {
    static constexpr std::string_view kExtensions[] = {".cpp", ".cc", ".cxx",
                                                       ".h",   ".hpp", ".hxx"};
    return std::ranges::find(kExtensions, path.extension().string()) != std::end(kExtensions);
}

int display_width(std::string_view chars, int tab_width) {
    int col = 0;
    for (char c : chars) {
        col += c == '\t' ? tab_width - col % tab_width : 1;
    }
    return col;
}

struct RoleCount {
    std::uint32_t mismatch = 0;
    // Continuation mismatches where aligning to the column after the open
    // bracket would have matched (the T2.5 alignment style).
    std::uint32_t align_fix = 0;
};

struct Report {
    std::uint32_t files = 0;
    std::uint32_t unclean = 0; // skipped: not clang-format-clean
    std::uint32_t lines = 0;
    std::uint32_t blank = 0;
    std::uint32_t preserved = 0;
    std::uint32_t checked = 0;
    std::uint32_t matched = 0;
    double parse_ms = 0;
    double indent_ms = 0;
    std::map<FormatRole, RoleCount> by_role;

    void add(const Report& other) {
        files += other.files;
        unclean += other.unclean;
        lines += other.lines;
        blank += other.blank;
        preserved += other.preserved;
        checked += other.checked;
        matched += other.matched;
        parse_ms += other.parse_ms;
        indent_ms += other.indent_ms;
        for (const auto& [role, count] : other.by_role) {
            by_role[role].mismatch += count.mismatch;
            by_role[role].align_fix += count.align_fix;
        }
    }
};

bool is_bracket_continuation(FormatRole role) {
    return role == FormatRole::ParenContinuation || role == FormatRole::BracketContinuation ||
           role == FormatRole::TemplateArgsContinuation;
}

void print_role_histogram(const Report& report) {
    std::vector<std::pair<FormatRole, RoleCount>> rows(report.by_role.begin(),
                                                       report.by_role.end());
    std::ranges::sort(rows, std::greater{},
                      [](const auto& row) { return row.second.mismatch; });
    for (const auto& [role, count] : rows) {
        std::string note;
        if (count.align_fix > 0) {
            note = std::format("  (open-bracket alignment would fix {})", count.align_fix);
        }
        std::cout << std::format("  {:<28} {:>6}{}\n", format_role_name(role), count.mismatch,
                                 note);
    }
}

void print_summary(const Report& report) {
    double accuracy = report.checked > 0 ? 100.0 * report.matched / report.checked : 100.0;
    std::cout << std::format(
        "lines {}  checked {}  matched {} ({:.2f}%)  blank {}  preserved {}\n", report.lines,
        report.checked, report.matched, accuracy, report.blank, report.preserved);
    std::cout << std::format("parse {:.1f}ms  indent {:.1f}ms\n", report.parse_ms,
                             report.indent_ms);
    if (report.checked > report.matched) {
        std::cout << "mismatches by role:\n";
        print_role_histogram(report);
    }
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

// True when clang-format (resolving .clang-format via --style=file) would
// leave the file byte-identical. Formatting failures count as clean so a
// missing tool degrades to an unfiltered run, with one warning.
bool clang_format_clean(const fs::path& path, std::string_view text) {
    const std::string cmd =
        "clang-format --style=file " + shell_quote(path.string()) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return true;
    }
    std::string out;
    char buf[1 << 16];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, pipe)) > 0) {
        out.append(buf, n);
    }
    if (pclose(pipe) != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::cerr << "indent-core: clang-format failed; --clean-only not filtering\n";
        }
        return true;
    }
    return out == text;
}

Report bench_file(const fs::path& path, const CppIndentStyle& style, const BenchOptions& options,
                  int& show_budget) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "indent-core: cannot open " << path.string() << "\n";
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    if (options.clean_only && !clang_format_clean(path, buffer.view())) {
        Report skipped;
        skipped.unclean = 1;
        return skipped;
    }

    Document document(buffer.str());
    DocumentSnapshot snapshot = document.snapshot();
    std::string_view text = snapshot.text();
    const LineIndex& lines = snapshot.lines();

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    SyntaxTree tree = parse(text);
    auto t1 = clock::now();

    Report report;
    report.files = 1;
    report.lines = lines.line_count();
    for (std::uint32_t line = 0; line < lines.line_count(); ++line) {
        TextRange content = lines.line_content_range(line);
        std::string_view line_text = text.substr(content.start.value, content.length());
        std::size_t ws = 0;
        while (ws < line_text.size() && (line_text[ws] == ' ' || line_text[ws] == '\t')) {
            ++ws;
        }
        if (ws == line_text.size()) {
            ++report.blank;
            continue;
        }

        IndentDecision decision = compute_line_indent(snapshot, tree, line, style);
        if (decision.preserve) {
            ++report.preserved;
            continue;
        }
        ++report.checked;
        const int actual = display_width(line_text.substr(0, ws), style.tab_width);
        if (actual == decision.target_column) {
            ++report.matched;
            continue;
        }

        RoleCount& count = report.by_role[decision.role];
        ++count.mismatch;
        if (is_bracket_continuation(decision.role) && decision.anchor) {
            TextOffset open = *decision.anchor;
            std::uint32_t open_line_start = lines.line_start(lines.position(open).line).value;
            int open_col = display_width(
                text.substr(open_line_start, open.value - open_line_start), style.tab_width);
            if (open_col + 1 == actual) {
                ++count.align_fix;
            }
        }
        if (show_budget > 0) {
            --show_budget;
            std::cout << std::format("{}:{}: {} expected {} actual {} | {}\n", path.string(),
                                     line + 1, format_role_name(decision.role),
                                     decision.target_column, actual,
                                     line_text.substr(ws, 60));
        }
    }
    auto t2 = clock::now();
    report.parse_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    report.indent_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    return report;
}

std::optional<CppIndentStyle> resolve_style(std::string_view preset) {
    CppIndentStyle style;
    if (preset == "default") {
        return style;
    }
    if (preset == "llvm") {
        style.indent_width = 2;
        style.continuation_indent = 4;
        style.brace_init_continuation = true;
        style.indent_wrapped_function_names = false;
        return style;
    }
    return std::nullopt;
}

} // namespace

int run_bench(const std::vector<std::string>& paths, const BenchOptions& options) {
    auto style = resolve_style(options.style_preset);
    if (!style) {
        std::cerr << "indent-core: unknown style preset '" << options.style_preset
                  << "' (expected: default, llvm)\n";
        return 2;
    }

    std::vector<fs::path> files;
    for (const std::string& raw : paths) {
        fs::path path(raw);
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(path, ec)) {
                if (entry.is_regular_file() && has_cpp_extension(entry.path())) {
                    files.push_back(entry.path());
                }
            }
        } else if (fs::is_regular_file(path, ec)) {
            files.push_back(path);
        } else {
            std::cerr << "indent-core: no such file or directory: " << raw << "\n";
            return 2;
        }
    }
    if (files.empty()) {
        std::cerr << "indent-core: no C++ sources found\n";
        return 2;
    }
    std::ranges::sort(files);

    int show_budget = options.show_mismatches;
    Report total;
    for (const fs::path& file : files) {
        Report report = bench_file(file, *style, options, show_budget);
        total.add(report);
        if (report.files == 0) {
            continue;
        }
        if (files.size() > 1) {
            std::cout << "=== " << file.string() << "\n";
        }
        print_summary(report);
    }
    if (total.files > 1 || total.unclean > 0) {
        std::cout << std::format("=== total ({} files{})\n", total.files,
                                 total.unclean > 0
                                     ? std::format(", skipped {} not format-clean",
                                                   total.unclean)
                                     : std::string());
        print_summary(total);
    }
    return total.files > 0 ? 0 : 1;
}

} // namespace cind
