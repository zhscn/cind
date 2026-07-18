#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cind {

enum class ChromeInteractionKind : std::uint8_t { Text, Picker };

struct ChromeItem {
    std::string label;
    std::string detail;

    friend bool operator==(const ChromeItem&, const ChromeItem&) = default;
};

struct ChromeHint {
    std::string key;
    std::string detail;
    bool prefix = false;
};

struct ChromeFacts {
    std::optional<ChromeInteractionKind> interaction;
    std::string prompt;
    std::string input;
    std::size_t input_caret = 0;
    std::vector<ChromeItem> candidates;
    std::optional<std::size_t> selection;
    std::string message;
    std::string preedit;
    std::string pending_sequence;
    std::string pending_prefix;
    std::vector<ChromeHint> hints;
};

struct ChromeContent {
    std::string pending_key;
    std::string echo;
    std::optional<std::size_t> echo_cursor_byte;
    std::optional<int> echo_cursor_column;
    std::string popup_title;
    std::vector<ChromeItem> popup_items;
    std::size_t popup_capacity = 0;
    std::optional<std::size_t> popup_selection;
    std::optional<std::string> popup_input;
    std::optional<std::size_t> popup_input_cursor;

    friend bool operator==(const ChromeContent&, const ChromeContent&) = default;
};

} // namespace cind
