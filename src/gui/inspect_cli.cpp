#include "gui/inspect_server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cind::gui {

namespace {

void print_usage() {
    std::fprintf(stderr, "usage: cind-ui-inspect [--socket PATH | --pid PID] COMMAND [ARGS...]\n"
                         "commands:\n"
                         "  list\n"
                         "  snapshot\n"
                         "  tree\n"
                         "  get <path>\n"
                         "  pick <window-x> <window-y>\n"
                         "  events [after-sequence]\n"
                         "  watch frames|events\n");
}

std::string join_arguments(const std::vector<std::string_view>& arguments, std::size_t first) {
    std::string output;
    for (std::size_t index = first; index < arguments.size(); ++index) {
        if (!output.empty()) {
            output.push_back(' ');
        }
        output += arguments[index];
    }
    return output;
}

std::filesystem::path select_socket(const std::optional<std::filesystem::path>& explicit_socket,
                                    const std::optional<int>& process_id) {
    if (explicit_socket) {
        return *explicit_socket;
    }
    if (process_id) {
        return default_inspector_socket_path(*process_id);
    }
    const std::vector<std::filesystem::path> sockets = discover_inspector_sockets();
    if (sockets.empty()) {
        throw std::runtime_error("no running cind GUI inspector was found");
    }
    if (sockets.size() != 1) {
        std::string message = "multiple cind GUI inspectors are running; use --pid or --socket:";
        for (const std::filesystem::path& socket : sockets) {
            message += std::format("\n  {}", socket.string());
        }
        throw std::runtime_error(message);
    }
    return sockets.front();
}

InspectionResponse request(const std::filesystem::path& socket, std::string_view command) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        InspectionResponse response = send_inspector_request(socket, command);
        if (response.ok) {
            return response;
        }
        if (response.payload != "no frame has been published") {
            throw std::runtime_error(response.payload);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    throw std::runtime_error("GUI inspector did not publish its initial frame");
}

void watch_frames(const std::filesystem::path& socket) {
    std::uint64_t last_frame = 0;
    while (true) {
        const std::uint64_t frame = std::stoull(request(socket, "get frame.id").payload);
        if (frame != last_frame) {
            std::puts(request(socket, "snapshot").payload.c_str());
            std::fflush(stdout);
            last_frame = frame;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void watch_events(const std::filesystem::path& socket) {
    std::uint64_t last_event = 0;
    while (true) {
        const std::uint64_t event = std::stoull(request(socket, "get event.last_sequence").payload);
        if (event > last_event) {
            std::puts(request(socket, std::format("events {}", last_event)).payload.c_str());
            std::fflush(stdout);
            last_event = event;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace

int inspect_main(int argc, char** argv) {
    std::optional<std::filesystem::path> explicit_socket;
    std::optional<int> process_id;
    std::vector<std::string_view> arguments;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--socket") {
            if (++index >= argc) {
                throw std::runtime_error("--socket requires a path");
            }
            explicit_socket = argv[index];
        } else if (argument == "--pid") {
            if (++index >= argc) {
                throw std::runtime_error("--pid requires a process id");
            }
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(argv[index], &end, 10);
            if (errno != 0 || !end || *end != '\0' || parsed <= 0 ||
                parsed > std::numeric_limits<int>::max()) {
                throw std::runtime_error("--pid requires a positive process id");
            }
            process_id = static_cast<int>(parsed);
        } else if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else {
            arguments.push_back(argument);
        }
    }

    if (explicit_socket && process_id) {
        throw std::runtime_error("--socket and --pid are mutually exclusive");
    }
    if (arguments.empty()) {
        print_usage();
        return 2;
    }
    if (arguments.front() == "list") {
        for (const std::filesystem::path& socket : discover_inspector_sockets()) {
            std::puts(socket.c_str());
        }
        return 0;
    }

    const std::filesystem::path socket = select_socket(explicit_socket, process_id);
    if (arguments.front() == "watch") {
        if (arguments.size() != 2) {
            throw std::runtime_error("usage: watch frames|events");
        }
        if (arguments[1] == "frames") {
            watch_frames(socket);
        } else if (arguments[1] == "events") {
            watch_events(socket);
        } else {
            throw std::runtime_error("usage: watch frames|events");
        }
        return 0;
    }

    const InspectionResponse response = request(socket, join_arguments(arguments, 0));
    std::fwrite(response.payload.data(), 1, response.payload.size(), stdout);
    if (response.payload.empty() || response.payload.back() != '\n') {
        std::fputc('\n', stdout);
    }
    return 0;
}

} // namespace cind::gui

int main(int argc, char** argv) {
    try {
        return cind::gui::inspect_main(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cind-ui-inspect: %s\n", error.what());
        return 1;
    }
}
