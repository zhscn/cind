#pragma once

#include "gui/inspection.hpp"

#include <atomic>
#include <filesystem>
#include <future>
#include <thread>
#include <vector>

namespace cind::gui {

std::filesystem::path inspector_runtime_directory();
std::filesystem::path default_inspector_socket_path(int process_id);
std::vector<std::filesystem::path> discover_inspector_sockets();

class InspectorServer {
public:
    InspectorServer(InspectionHub& hub, std::filesystem::path socket_path);
    ~InspectorServer();

    InspectorServer(const InspectorServer&) = delete;
    InspectorServer& operator=(const InspectorServer&) = delete;

    const std::filesystem::path& socket_path() const { return socket_path_; }

private:
    void serve();
    void handle_client(int client) const;

    InspectionHub& hub_;
    std::filesystem::path socket_path_;
    std::atomic_int socket_ = -1;
    std::atomic_bool stopping_ = false;
    std::thread thread_;
    std::vector<std::future<void>> clients_;
};

InspectionResponse send_inspector_request(const std::filesystem::path& socket_path,
                                          std::string_view request);

} // namespace cind::gui
