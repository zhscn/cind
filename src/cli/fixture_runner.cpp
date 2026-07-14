#include "cli/fixture_runner.hpp"

#include "cli/session.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

namespace cind {

namespace {

// Fixture format (design.md §13.3) — the supported YAML subset:
//
//   name: optional display name
//   style:
//     indent_namespace_body: true
//   initial: |
//     namespace foo {^
//     }
//   actions:
//     - enter
//     - type: "int x;"
//     - indent
//     - undo
//     - redo
//   expected: |
//     namespace foo {
//     int x;^
//     }
//
// Block scalars use two-space indentation; '^' marks the caret (required in
// initial, optional in expected). Lines starting with '#' are comments.

struct FixtureAction {
    enum class Kind { Enter, Indent, Undo, Redo, Type };
    Kind kind = Kind::Enter;
    std::string text;
};

struct Fixture {
    std::string name;
    CppIndentStyle style;
    std::string initial;
    TextOffset caret;
    std::vector<FixtureAction> actions;
    std::string expected;
    std::optional<TextOffset> expected_caret;
};

std::vector<std::string_view> split_lines(std::string_view content) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos) {
            if (start < content.size()) {
                lines.push_back(content.substr(start));
            }
            break;
        }
        lines.push_back(content.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// Collects a two-space-indented block starting at lines[i]; empty lines stay
// in the block when more indented content follows.
std::string collect_block(const std::vector<std::string_view>& lines, std::size_t& i) {
    std::string block;
    while (i < lines.size()) {
        std::string_view line = lines[i];
        if (line.starts_with("  ")) {
            block += line.substr(2);
            block += '\n';
            ++i;
            continue;
        }
        if (line.empty()) {
            std::size_t j = i;
            while (j < lines.size() && lines[j].empty()) {
                ++j;
            }
            if (j < lines.size() && lines[j].starts_with("  ")) {
                block.append(j - i, '\n');
                i = j;
                continue;
            }
        }
        break;
    }
    return block;
}

std::optional<std::string> parse_quoted(std::string_view s) {
    std::size_t open = s.find('"');
    std::size_t close = s.rfind('"');
    if (open == std::string_view::npos || close <= open) {
        return std::nullopt;
    }
    std::string_view body = s.substr(open + 1, close - open - 1);
    std::string out;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size() &&
            (body[i + 1] == '"' || body[i + 1] == '\\')) {
            ++i;
        }
        out += body[i];
    }
    return out;
}

std::optional<Fixture> parse_fixture(std::string_view content, std::string& error) {
    Fixture fixture;
    bool have_initial = false;
    bool have_expected = false;
    auto lines = split_lines(content);

    std::size_t i = 0;
    while (i < lines.size()) {
        std::string_view line = lines[i];
        if (line.empty() || line.starts_with('#')) {
            ++i;
            continue;
        }
        if (line.starts_with("name: ")) {
            fixture.name = std::string(line.substr(6));
            ++i;
            continue;
        }
        if (line == "style:") {
            ++i;
            while (i < lines.size() && lines[i].starts_with("  ")) {
                std::string_view entry = lines[i].substr(2);
                std::size_t colon = entry.find(": ");
                if (colon == std::string_view::npos ||
                    !set_style_field(fixture.style, entry.substr(0, colon),
                                     entry.substr(colon + 2))) {
                    error = "bad style entry: " + std::string(entry);
                    return std::nullopt;
                }
                ++i;
            }
            continue;
        }
        if (line == "initial: |") {
            ++i;
            CaretText split = split_caret_marker(collect_block(lines, i));
            if (!split.had_marker) {
                error = "initial block has no '^' caret marker";
                return std::nullopt;
            }
            fixture.initial = std::move(split.text);
            fixture.caret = split.caret;
            have_initial = true;
            continue;
        }
        if (line == "expected: |") {
            ++i;
            CaretText split = split_caret_marker(collect_block(lines, i));
            fixture.expected = std::move(split.text);
            if (split.had_marker) {
                fixture.expected_caret = split.caret;
            }
            have_expected = true;
            continue;
        }
        if (line == "actions:") {
            ++i;
            while (i < lines.size() && lines[i].starts_with("  - ")) {
                std::string_view action = lines[i].substr(4);
                FixtureAction parsed;
                if (action == "enter") {
                    parsed.kind = FixtureAction::Kind::Enter;
                } else if (action == "indent") {
                    parsed.kind = FixtureAction::Kind::Indent;
                } else if (action == "undo") {
                    parsed.kind = FixtureAction::Kind::Undo;
                } else if (action == "redo") {
                    parsed.kind = FixtureAction::Kind::Redo;
                } else if (action.starts_with("type: ")) {
                    auto text = parse_quoted(action.substr(6));
                    if (!text) {
                        error = "bad type action: " + std::string(action);
                        return std::nullopt;
                    }
                    parsed.kind = FixtureAction::Kind::Type;
                    parsed.text = std::move(*text);
                } else {
                    error = "unknown action: " + std::string(action);
                    return std::nullopt;
                }
                fixture.actions.push_back(std::move(parsed));
                ++i;
            }
            continue;
        }
        error = "unrecognized line: " + std::string(line);
        return std::nullopt;
    }

    if (!have_initial || !have_expected) {
        error = "fixture needs both 'initial: |' and 'expected: |' blocks";
        return std::nullopt;
    }
    return fixture;
}

bool run_one(const Fixture& fixture, std::string& failure) {
    EditSession session(fixture.initial, fixture.style);
    session.set_caret(fixture.caret);

    for (const FixtureAction& action : fixture.actions) {
        switch (action.kind) {
        case FixtureAction::Kind::Enter: session.enter(); break;
        case FixtureAction::Kind::Indent: session.indent(); break;
        case FixtureAction::Kind::Type: session.type_text(action.text); break;
        case FixtureAction::Kind::Undo:
            if (!session.undo()) {
                failure = "undo with empty undo stack";
                return false;
            }
            break;
        case FixtureAction::Kind::Redo:
            if (!session.redo()) {
                failure = "redo with empty redo stack";
                return false;
            }
            break;
        }
    }

    std::string actual = session.snapshot().content().to_string();
    bool text_ok = actual == fixture.expected;
    bool caret_ok = !fixture.expected_caret || session.caret() == *fixture.expected_caret;
    if (text_ok && caret_ok) {
        return true;
    }

    std::string rendered = fixture.expected_caret ? session.render_with_caret() : actual;
    std::string expected_rendered(fixture.expected);
    if (fixture.expected_caret) {
        expected_rendered.insert(fixture.expected_caret->value, "^");
    }
    failure = "--- expected ---\n" + expected_rendered + "--- actual ---\n" + rendered;
    if (!failure.ends_with('\n')) {
        failure += '\n';
    }
    return false;
}

} // namespace

int run_fixtures(const char* path) {
    namespace fs = std::filesystem;

    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(path, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
    } else {
        files.emplace_back(path);
    }
    if (files.empty()) {
        std::cerr << "indent-core: no .yaml fixtures under " << path << "\n";
        return 1;
    }

    int failed = 0;
    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::cerr << "indent-core: cannot open " << file.string() << "\n";
            ++failed;
            continue;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();

        std::string error;
        auto fixture = parse_fixture(buffer.str(), error);
        if (!fixture) {
            std::cout << "FAIL " << file.string() << " (parse error: " << error << ")\n";
            ++failed;
            continue;
        }
        std::string display =
            fixture->name.empty() ? file.filename().string() : fixture->name;

        std::string failure;
        bool ok = false;
        try {
            ok = run_one(*fixture, failure);
        } catch (const std::exception& e) {
            failure = std::string("exception: ") + e.what() + "\n";
        }
        if (ok) {
            std::cout << "PASS " << display << "\n";
        } else {
            std::cout << "FAIL " << display << " (" << file.string() << ")\n" << failure;
            ++failed;
        }
    }

    std::cout << files.size() - static_cast<std::size_t>(failed) << " passed, " << failed
              << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace cind
