#pragma once

#include "document/text.hpp"

#include <filesystem>
#include <system_error>

namespace cind {

// Writes a complete buffer through a same-directory temporary file, then
// publishes it with rename and synchronizes the containing directory.
std::error_code save_file_atomically(const std::filesystem::path& path, const Text& content);

} // namespace cind
