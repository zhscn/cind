#include "cli/style_loader.hpp"

#include "formatting/clang_format_style.hpp"

#include <fstream>
#include <sstream>

namespace cind {

namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::optional<fs::path> find_from(fs::path dir) {
    std::error_code ec;
    dir = fs::absolute(dir, ec);
    for (; !dir.empty(); dir = dir.parent_path()) {
        for (std::string_view name : {".clang-format", "_clang-format"}) {
            fs::path candidate = dir / name;
            if (fs::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
        if (dir == dir.root_path()) {
            break;
        }
    }
    return std::nullopt;
}

// Resolve one config file, following InheritParentConfig upward. `depth`
// bounds pathological chains.
std::optional<LoadedStyle> load_file(const fs::path& config, int depth) {
    std::optional<std::string> text = read_file(config);
    if (!text) {
        return std::nullopt;
    }
    // clang-format's getStyle applies a config file on top of the LLVM
    // fallback style; a file without BasedOnStyle means "LLVM plus these".
    CppIndentStyle base;
    apply_clang_format_preset("LLVM", base);
    ClangFormatStyle parsed = parse_clang_format_yaml(*text, base);
    if (parsed.inherit_parent && depth < 16) {
        if (auto parent = find_from(config.parent_path().parent_path())) {
            if (auto inherited = load_file(*parent, depth + 1)) {
                base = inherited->style;
                parsed = parse_clang_format_yaml(*text, base);
                parsed.warnings.insert(parsed.warnings.begin(), inherited->warnings.begin(),
                                       inherited->warnings.end());
            }
        }
    }
    return LoadedStyle{parsed.style, std::move(parsed.warnings), config, parsed.disable_format};
}

} // namespace

std::optional<fs::path> find_clang_format_file(fs::path start_dir) {
    return find_from(std::move(start_dir));
}

std::optional<LoadedStyle> load_clang_format_style(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        auto config = find_from(path);
        if (!config) {
            return std::nullopt;
        }
        return load_file(*config, 0);
    }
    return load_file(path, 0);
}

} // namespace cind
