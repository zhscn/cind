#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace cind {

class JsonRpcFramer {
public:
    std::expected<std::vector<std::string>, std::string> push(std::string_view bytes);
    void reset();

private:
    std::string buffer_;
    std::size_t body_size_ = 0;
    bool reading_body_ = false;
};

std::string frame_json_rpc(std::string_view json);

} // namespace cind
