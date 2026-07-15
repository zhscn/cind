#pragma once

#include "document/text.hpp"

#include <filesystem>
#include <system_error>

namespace cind {

// Writes a complete buffer through an exclusive same-directory temporary
// file, preserves an existing regular file's mode, then publishes it with
// rename and synchronizes the containing directory. Symbolic-link and
// non-regular destinations are rejected.
std::error_code save_file_atomically(const std::filesystem::path& path, const Text& content);

} // namespace cind
