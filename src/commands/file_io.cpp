#include "commands/file_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <format>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cind {

namespace {

std::error_code system_error(int error = errno) {
    return {error, std::generic_category()};
}

void close_ignoring_errors(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~FileDescriptor() { close_ignoring_errors(descriptor_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const { return descriptor_; }
    int close() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return ::close(descriptor);
    }

private:
    int descriptor_;
};

} // namespace

std::expected<FileReadResult, std::error_code>
read_file_contents(const std::filesystem::path& path, const std::stop_token& cancellation) {
    if (cancellation.stop_requested()) {
        return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return FileReadResult{};
        }
        return std::unexpected(system_error());
    }
    FileDescriptor file(fd);

    struct stat status{};
    if (::fstat(file.get(), &status) != 0) {
        return std::unexpected(system_error());
    }
    if (S_ISDIR(status.st_mode)) {
        return std::unexpected(std::make_error_code(std::errc::is_a_directory));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }

    FileReadResult result{.exists = true, .contents = {}};
    if (status.st_size > 0 && static_cast<std::uintmax_t>(status.st_size) <=
                                  static_cast<std::uintmax_t>(result.contents.max_size())) {
        result.contents.reserve(static_cast<std::size_t>(status.st_size));
    }
    char buffer[64 * 1024];
    while (true) {
        if (cancellation.stop_requested()) {
            return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }
        const ssize_t received = ::read(file.get(), buffer, sizeof(buffer));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            return std::unexpected(system_error());
        }
        if (received == 0) {
            break;
        }
        result.contents.append(buffer, static_cast<std::size_t>(received));
    }
    if (file.close() != 0) {
        return std::unexpected(system_error());
    }
    return result;
}

std::expected<DirectoryListing, std::error_code>
list_directory(const std::filesystem::path& path, std::size_t maximum_entries,
               const std::stop_token& cancellation) {
    namespace fs = std::filesystem;
    if (cancellation.stop_requested()) {
        return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    }
    std::error_code error;
    fs::path directory = fs::absolute(path, error).lexically_normal();
    if (error) {
        return std::unexpected(error);
    }
    if (!fs::is_directory(directory, error)) {
        return std::unexpected(error ? error : std::make_error_code(std::errc::not_a_directory));
    }

    DirectoryListing result{.directory = std::move(directory), .entries = {}};
    result.entries.reserve(std::min<std::size_t>(maximum_entries, 256));
    for (fs::directory_iterator iterator(result.directory, error), end;
         !error && iterator != end && result.entries.size() < maximum_entries;
         iterator.increment(error)) {
        if (cancellation.stop_requested()) {
            return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }
        const fs::directory_entry& entry = *iterator;
        const bool directory_entry = entry.is_directory(error);
        if (error) {
            break;
        }
        result.entries.push_back({.path = entry.path().lexically_normal(),
                                  .name = entry.path().filename().string(),
                                  .directory = directory_entry});
    }
    if (error) {
        return std::unexpected(error);
    }
    return result;
}

std::error_code save_file_atomically(const std::filesystem::path& path, const Text& content,
                                     const std::stop_token& cancellation) {
    namespace fs = std::filesystem;
    if (cancellation.stop_requested()) {
        return std::make_error_code(std::errc::operation_canceled);
    }
    const fs::path directory = path.has_parent_path() ? path.parent_path() : fs::path(".");
    const fs::path filename = path.filename();
    if (filename.empty()) {
        return std::make_error_code(std::errc::invalid_argument);
    }

    const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        return system_error();
    }

    struct stat destination{};
    bool destination_exists = false;
    if (::fstatat(directory_fd, filename.c_str(), &destination, AT_SYMLINK_NOFOLLOW) == 0) {
        destination_exists = true;
        if (!S_ISREG(destination.st_mode)) {
            close_ignoring_errors(directory_fd);
            return std::make_error_code(S_ISLNK(destination.st_mode)
                                            ? std::errc::too_many_symbolic_link_levels
                                            : std::errc::operation_not_supported);
        }
    } else if (errno != ENOENT) {
        const std::error_code error = system_error();
        close_ignoring_errors(directory_fd);
        return error;
    }

    static std::atomic_uint64_t next_temporary_id = 1;
    std::string temporary;
    int fd = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        temporary = std::format(".cind-tmp-{}-{}", static_cast<long>(::getpid()),
                                next_temporary_id.fetch_add(1, std::memory_order_relaxed));
        fd = ::openat(directory_fd, temporary.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      destination_exists ? 0600 : 0666);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            const std::error_code error = system_error();
            close_ignoring_errors(directory_fd);
            return error;
        }
    }
    if (fd < 0) {
        close_ignoring_errors(directory_fd);
        return std::make_error_code(std::errc::file_exists);
    }

    for (TextCursor cursor(content); !cursor.at_end(); cursor.advance_chunk()) {
        std::string_view chunk = cursor.chunk();
        while (!chunk.empty()) {
            if (cancellation.stop_requested()) {
                close_ignoring_errors(fd);
                ::unlinkat(directory_fd, temporary.c_str(), 0);
                close_ignoring_errors(directory_fd);
                return std::make_error_code(std::errc::operation_canceled);
            }
            const ssize_t written = ::write(fd, chunk.data(), chunk.size());
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                const std::error_code error =
                    written < 0 ? system_error() : std::make_error_code(std::errc::io_error);
                close_ignoring_errors(fd);
                ::unlinkat(directory_fd, temporary.c_str(), 0);
                close_ignoring_errors(directory_fd);
                return error;
            }
            chunk.remove_prefix(static_cast<std::size_t>(written));
        }
    }

    int error = 0;
    if (destination_exists && ::fchmod(fd, static_cast<mode_t>(destination.st_mode & 07777)) != 0) {
        error = errno;
    }
    if (::fsync(fd) != 0) {
        if (error == 0) {
            error = errno;
        }
    }
    if (::close(fd) != 0 && error == 0) {
        error = errno;
    }
    if (error != 0) {
        ::unlinkat(directory_fd, temporary.c_str(), 0);
        close_ignoring_errors(directory_fd);
        return system_error(error);
    }

    if (cancellation.stop_requested()) {
        ::unlinkat(directory_fd, temporary.c_str(), 0);
        close_ignoring_errors(directory_fd);
        return std::make_error_code(std::errc::operation_canceled);
    }

    if (::renameat(directory_fd, temporary.c_str(), directory_fd, filename.c_str()) != 0) {
        const std::error_code rename_error = system_error();
        ::unlinkat(directory_fd, temporary.c_str(), 0);
        close_ignoring_errors(directory_fd);
        return rename_error;
    }

    if (::fsync(directory_fd) != 0) {
        const std::error_code sync_error = system_error();
        close_ignoring_errors(directory_fd);
        return sync_error;
    }
    if (::close(directory_fd) != 0) {
        return system_error();
    }
    return {};
}

} // namespace cind
