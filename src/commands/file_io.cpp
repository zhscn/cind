#include "commands/file_io.hpp"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace cind {

std::error_code save_file_atomically(const std::filesystem::path& path, const Text& content) {
    namespace fs = std::filesystem;
    const fs::path temporary = path.string() + ".cind-tmp";

    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return std::error_code(errno, std::generic_category());
    }
    for (TextCursor cursor(content); !cursor.at_end(); cursor.advance_chunk()) {
        std::string_view chunk = cursor.chunk();
        while (!chunk.empty()) {
            const ssize_t written = ::write(fd, chunk.data(), chunk.size());
            if (written <= 0) {
                const int error = errno;
                ::close(fd);
                ::unlink(temporary.c_str());
                return std::error_code(error, std::generic_category());
            }
            chunk.remove_prefix(static_cast<std::size_t>(written));
        }
    }
    int error = 0;
    if (::fsync(fd) != 0) {
        error = errno;
    }
    if (::close(fd) != 0 && error == 0) {
        error = errno;
    }
    if (error != 0) {
        ::unlink(temporary.c_str());
        return std::error_code(error, std::generic_category());
    }

    std::error_code rename_error;
    fs::rename(temporary, path, rename_error);
    if (rename_error) {
        ::unlink(temporary.c_str());
        return rename_error;
    }

    const fs::path directory = path.has_parent_path() ? path.parent_path() : fs::path(".");
    const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory_fd >= 0) {
        ::fsync(directory_fd);
        ::close(directory_fd);
    }
    return {};
}

} // namespace cind
