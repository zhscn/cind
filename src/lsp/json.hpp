#pragma once

#include <glaze/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cind::lsp_json {

// LSP request ids and document versions must retain their full integer range.
using Json = glz::generic_u64;

inline std::expected<Json, std::string> parse(std::string_view input,
                                             std::string_view description = "JSON") {
    std::expected<Json, glz::error_ctx> parsed = glz::read_json<Json>(input);
    if (!parsed) {
        return std::unexpected(std::format("{} is not valid JSON: {}", description,
                                           glz::format_error(parsed.error(), input)));
    }
    return std::move(*parsed);
}

inline std::string dump(const Json& value) {
    std::expected<std::string, glz::error_ctx> serialized = glz::write_json(value);
    if (!serialized) {
        throw std::runtime_error(
            std::format("cannot serialize JSON: {}", glz::format_error(serialized.error())));
    }
    return std::move(*serialized);
}

inline const Json* find(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto& values = object.get_object();
    const auto found = values.find(key);
    return found == values.end() ? nullptr : &found->second;
}

inline Json* find(Json& object, std::string_view key) {
    return const_cast<Json*>(find(std::as_const(object), key));
}

inline bool equal(const Json& left, const Json& right) {
    if (left.is_object() && right.is_object()) {
        if (left.size() != right.size()) {
            return false;
        }
        for (const auto& [key, value] : left.get_object()) {
            const Json* candidate = find(right, key);
            if (candidate == nullptr || !equal(value, *candidate)) {
                return false;
            }
        }
        return true;
    }
    if (left.is_array() && right.is_array()) {
        if (left.size() != right.size()) {
            return false;
        }
        const auto& left_values = left.get_array();
        const auto& right_values = right.get_array();
        for (std::size_t index = 0; index < left_values.size(); ++index) {
            if (!equal(left_values[index], right_values[index])) {
                return false;
            }
        }
        return true;
    }
    if (left.is_uint64() && right.is_uint64()) {
        return left.get<std::uint64_t>() == right.get<std::uint64_t>();
    }
    if (left.is_int64() && right.is_int64()) {
        return left.get<std::int64_t>() == right.get<std::int64_t>();
    }
    if (left.is_uint64() && right.is_int64()) {
        const std::int64_t signed_value = right.get<std::int64_t>();
        return signed_value >= 0 &&
               left.get<std::uint64_t>() == static_cast<std::uint64_t>(signed_value);
    }
    if (left.is_int64() && right.is_uint64()) {
        return equal(right, left);
    }
    if (left.is_double() && right.is_number()) {
        return left.get<double>() == right.as<double>();
    }
    if (right.is_double() && left.is_number()) {
        return left.as<double>() == right.get<double>();
    }
    if (left.is_string() && right.is_string()) {
        return left.get<std::string>() == right.get<std::string>();
    }
    if (left.is_boolean() && right.is_boolean()) {
        return left.get<bool>() == right.get<bool>();
    }
    return left.is_null() && right.is_null();
}

inline std::optional<std::string> string(const Json& value) {
    return value.is_string() ? std::optional(value.get<std::string>()) : std::nullopt;
}

inline std::optional<bool> boolean(const Json& value) {
    return value.is_boolean() ? std::optional(value.get<bool>()) : std::nullopt;
}

inline std::optional<std::uint64_t> uint64(const Json& value) {
    return value.is_uint64() ? std::optional(value.get<std::uint64_t>()) : std::nullopt;
}

inline std::optional<std::int64_t> int64(const Json& value) {
    if (value.is_int64()) {
        return value.get<std::int64_t>();
    }
    if (value.is_uint64() &&
        value.get<std::uint64_t>() <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(value.get<std::uint64_t>());
    }
    return std::nullopt;
}

template <class T>
T value_or(const Json& object, std::string_view key, T fallback) {
    const Json* member = find(object, key);
    if (member == nullptr) {
        return fallback;
    }
    T result{};
    if (glz::read_json(result, *member)) {
        return fallback;
    }
    return result;
}

inline Json parse_or_throw(std::string_view input, std::string_view description = "JSON") {
    std::expected<Json, std::string> value = parse(input, description);
    if (!value) {
        throw std::runtime_error(value.error());
    }
    return std::move(*value);
}

} // namespace cind::lsp_json
