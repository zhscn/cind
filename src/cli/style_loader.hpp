#pragma once

#include "formatting/cpp_indent_style.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cind {

// Upward search for `.clang-format` / `_clang-format` starting at `start_dir`,
// same order clang-format uses.
std::optional<std::filesystem::path> find_clang_format_file(std::filesystem::path start_dir);

struct LoadedStyle {
    CppIndentStyle style;
    std::vector<std::string> warnings;
    std::filesystem::path config_path;
    bool disable_format = false;
};

// `path` may be a config file or a directory to search upward from.
// `BasedOnStyle: InheritParentConfig` resolves parent configs recursively.
// nullopt when no config file is found or it cannot be read.
std::optional<LoadedStyle> load_clang_format_style(const std::filesystem::path& path);

} // namespace cind
