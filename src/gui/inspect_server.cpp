#include "gui/inspect_server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace cind::gui {

namespace {

std::runtime_error socket_error(std::string_view operation) {
    return std::runtime_error(std::format("{}: {}", operation, std::strerror(errno)));
}

sockaddr_un socket_address(const std::filesystem::path& path) {
    const std::string value = path.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (value.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error(std::format("inspector socket path is too long: {}", value));
    }
    std::memcpy(address.sun_path, value.c_str(), value.size() + 1);
    return address;
}

void send_all(int socket, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t sent = ::send(socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("inspector socket write failed");
        }
        if (sent == 0) {
            throw std::runtime_error("inspector socket closed during write");
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

std::string receive_request_line(int socket) {
    std::string request;
    request.reserve(128);
    char buffer[512];
    while (request.size() <= 4096) {
        const ssize_t received = ::recv(socket, buffer, sizeof(buffer), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("inspector socket read failed");
        }
        if (received == 0) {
            break;
        }
        request.append(buffer, static_cast<std::size_t>(received));
        if (const std::size_t newline = request.find('\n'); newline != std::string::npos) {
            request.resize(newline);
            return request;
        }
    }
    if (request.size() > 4096) {
        throw std::runtime_error("inspector request exceeds 4096 bytes");
    }
    return request;
}

std::string receive_line(int socket) {
    constexpr std::size_t limit = 128;
    std::string line;
    while (line.size() <= limit) {
        char byte = 0;
        const ssize_t received = ::recv(socket, &byte, 1, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("inspector response read failed");
        }
        if (received == 0) {
            throw std::runtime_error("inspector socket closed before response header");
        }
        if (byte == '\n') {
            return line;
        }
        line.push_back(byte);
    }
    throw std::runtime_error("inspector response header is too long");
}

std::string receive_exact(int socket, std::size_t size) {
    std::string payload(size, '\0');
    std::size_t at = 0;
    while (at < size) {
        const ssize_t received = ::recv(socket, payload.data() + at, size - at, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw socket_error("inspector response read failed");
        }
        if (received == 0) {
            throw std::runtime_error("inspector socket closed before response payload");
        }
        at += static_cast<std::size_t>(received);
    }
    return payload;
}

} // namespace

std::filesystem::path inspector_runtime_directory() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
        return std::filesystem::path(runtime) / "cind";
    }
    return std::filesystem::temp_directory_path() /
           std::format("cind-{}", static_cast<unsigned long>(::getuid()));
}

std::filesystem::path default_inspector_socket_path(int process_id) {
    return inspector_runtime_directory() / std::format("ui-{}.sock", process_id);
}

std::vector<std::filesystem::path> discover_inspector_sockets() {
    std::vector<std::filesystem::path> sockets;
    const std::filesystem::path directory = inspector_runtime_directory();
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return sockets;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (entry.path().extension() == ".sock") {
            sockets.push_back(entry.path());
        }
    }
    std::ranges::sort(sockets);
    return sockets;
}

InspectorServer::InspectorServer(InspectionHub& hub, std::filesystem::path socket_path)
    : hub_(hub), socket_path_(std::move(socket_path)) {
    socket_path_ = std::filesystem::absolute(socket_path_);
    std::error_code error;
    std::filesystem::create_directories(socket_path_.parent_path(), error);
    if (error) {
        throw std::runtime_error(
            std::format("cannot create inspector runtime directory: {}", error.message()));
    }
    if (socket_path_.parent_path() == std::filesystem::absolute(inspector_runtime_directory())) {
        std::filesystem::permissions(socket_path_.parent_path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
        if (error) {
            throw std::runtime_error(
                std::format("cannot secure inspector runtime directory: {}", error.message()));
        }
    }

    struct stat existing{};
    if (::lstat(socket_path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            throw std::runtime_error(std::format("inspector path exists and is not a socket: {}",
                                                 socket_path_.string()));
        }
        if (::unlink(socket_path_.c_str()) != 0) {
            throw socket_error("cannot remove stale inspector socket");
        }
    } else if (errno != ENOENT) {
        throw socket_error("cannot inspect existing inspector socket");
    }

    const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket < 0) {
        throw socket_error("cannot create inspector socket");
    }
    socket_.store(socket);
    try {
        const sockaddr_un address = socket_address(socket_path_);
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw socket_error("cannot bind inspector socket");
        }
        if (::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
            throw socket_error("cannot secure inspector socket");
        }
        if (::listen(socket, 8) != 0) {
            throw socket_error("cannot listen on inspector socket");
        }
        thread_ = std::thread(&InspectorServer::serve, this);
    } catch (...) {
        ::close(socket_.exchange(-1));
        ::unlink(socket_path_.c_str());
        throw;
    }
}

InspectorServer::~InspectorServer() {
    stopping_.store(true);
    const int socket = socket_.exchange(-1);
    if (socket >= 0) {
        ::shutdown(socket, SHUT_RDWR);
        ::close(socket);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    clients_.clear(); // async futures join their workers
    ::unlink(socket_path_.c_str());
}

void InspectorServer::serve() {
    while (!stopping_.load()) {
        const int listening = socket_.load();
        if (listening < 0) {
            return;
        }
        const int client = ::accept4(listening, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (stopping_.load() || errno == EBADF || errno == EINVAL) {
                return;
            }
            continue;
        }
        const timeval timeout{.tv_sec = 1, .tv_usec = 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::erase_if(clients_, [](std::future<void>& worker) {
            return worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        try {
            clients_.push_back(std::async(std::launch::async, [this, client] {
                handle_client(client);
                ::close(client);
            }));
        } catch (...) {
            ::close(client);
            continue;
        }
    }
}

void InspectorServer::handle_client(int client) const {
    try {
        const std::string request = receive_request_line(client);
        const InspectionResponse response = run_inspection_query(hub_, request);
        const std::string header =
            std::format("{} {}\n", response.ok ? "OK" : "ERR", response.payload.size());
        send_all(client, header);
        send_all(client, response.payload);
    } catch (const std::exception& error) {
        const std::string payload = error.what();
        try {
            send_all(client, std::format("ERR {}\n", payload.size()));
            send_all(client, payload);
        } catch (...) {
            return;
        }
    }
}

InspectionResponse send_inspector_request(const std::filesystem::path& socket_path,
                                          std::string_view request) {
    const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket < 0) {
        throw socket_error("cannot create inspector client socket");
    }
    try {
        const sockaddr_un address = socket_address(socket_path);
        if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw socket_error(std::format("cannot connect to {}", socket_path.string()));
        }
        std::string line(request);
        line.push_back('\n');
        send_all(socket, line);
        ::shutdown(socket, SHUT_WR);

        const std::string header = receive_line(socket);
        const std::size_t separator = header.find(' ');
        if (separator == std::string::npos) {
            throw std::runtime_error("invalid inspector response header");
        }
        const std::string status = header.substr(0, separator);
        std::size_t payload_size = 0;
        try {
            payload_size = std::stoull(header.substr(separator + 1));
        } catch (const std::exception&) {
            throw std::runtime_error("invalid inspector response size");
        }
        constexpr std::size_t max_payload_size = 64ULL * 1024ULL * 1024ULL;
        if (payload_size > max_payload_size) {
            throw std::runtime_error("inspector response exceeds 64 MiB");
        }
        InspectionResponse response{status == "OK", receive_exact(socket, payload_size)};
        if (status != "OK" && status != "ERR") {
            throw std::runtime_error("invalid inspector response status");
        }
        ::close(socket);
        return response;
    } catch (...) {
        ::close(socket);
        throw;
    }
}

} // namespace cind::gui
