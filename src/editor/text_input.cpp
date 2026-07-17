#include "editor/text_input.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace cind {

namespace {

struct CodePoint {
    utf8proc_int32_t value = 0xFFFD;
    std::size_t bytes = 1;
};

CodePoint decode(std::string_view text) {
    utf8proc_int32_t value = 0;
    const auto available = static_cast<utf8proc_ssize_t>(std::min(
        text.size(), static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())));
    const utf8proc_ssize_t bytes =
        utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data()), available, &value);
    if (bytes <= 0) {
        return {};
    }
    return {.value = value, .bytes = static_cast<std::size_t>(bytes)};
}

std::size_t next_grapheme(std::string_view text, std::size_t caret) {
    if (caret >= text.size()) {
        return text.size();
    }
    const CodePoint first = decode(text.substr(caret));
    std::size_t next = caret + first.bytes;
    utf8proc_int32_t state = 0;
    utf8proc_int32_t previous = first.value;
    while (next < text.size()) {
        const CodePoint current = decode(text.substr(next));
        if (utf8proc_grapheme_break_stateful(previous, current.value, &state) != 0) {
            break;
        }
        next += current.bytes;
        previous = current.value;
    }
    return next;
}

std::size_t previous_grapheme(std::string_view text, std::size_t caret) {
    caret = std::min(caret, text.size());
    std::size_t previous = 0;
    std::size_t current = 0;
    while (current < caret) {
        previous = current;
        current = next_grapheme(text, current);
    }
    return previous;
}

bool word_character(utf8proc_int32_t value) {
    switch (utf8proc_category(value)) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_MC:
    case UTF8PROC_CATEGORY_ME:
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
    case UTF8PROC_CATEGORY_PC:
        return true;
    default:
        return false;
    }
}

bool grapheme_is_word(std::string_view text, std::size_t caret) {
    return caret < text.size() && word_character(decode(text.substr(caret)).value);
}

} // namespace

TextInput::TextInput(std::string text) : text_(std::move(text)), caret_(text_.size()) {}

bool TextInput::insert(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    text_.insert(caret_, text);
    caret_ += text.size();
    return true;
}

bool TextInput::erase_backward() {
    if (caret_ == 0) {
        return false;
    }
    const std::size_t start = previous_grapheme(text_, caret_);
    text_.erase(start, caret_ - start);
    caret_ = start;
    return true;
}

bool TextInput::erase_forward() {
    if (caret_ >= text_.size()) {
        return false;
    }
    text_.erase(caret_, next_grapheme(text_, caret_) - caret_);
    return true;
}

bool TextInput::move_backward() {
    const std::size_t target = previous_grapheme(text_, caret_);
    if (target == caret_) {
        return false;
    }
    caret_ = target;
    return true;
}

bool TextInput::move_forward() {
    const std::size_t target = next_grapheme(text_, caret_);
    if (target == caret_) {
        return false;
    }
    caret_ = target;
    return true;
}

bool TextInput::move_word_backward() {
    std::size_t target = caret_;
    while (target > 0) {
        const std::size_t previous = previous_grapheme(text_, target);
        if (grapheme_is_word(text_, previous)) {
            break;
        }
        target = previous;
    }
    while (target > 0) {
        const std::size_t previous = previous_grapheme(text_, target);
        if (!grapheme_is_word(text_, previous)) {
            break;
        }
        target = previous;
    }
    if (target == caret_) {
        return false;
    }
    caret_ = target;
    return true;
}

bool TextInput::move_word_forward() {
    std::size_t target = caret_;
    while (target < text_.size() && !grapheme_is_word(text_, target)) {
        target = next_grapheme(text_, target);
    }
    while (target < text_.size() && grapheme_is_word(text_, target)) {
        target = next_grapheme(text_, target);
    }
    if (target == caret_) {
        return false;
    }
    caret_ = target;
    return true;
}

bool TextInput::move_to_start() {
    if (caret_ == 0) {
        return false;
    }
    caret_ = 0;
    return true;
}

bool TextInput::move_to_end() {
    if (caret_ == text_.size()) {
        return false;
    }
    caret_ = text_.size();
    return true;
}

std::optional<std::string> TextInput::take_to_end() {
    if (caret_ == text_.size()) {
        return std::nullopt;
    }
    std::string removed = text_.substr(caret_);
    text_.erase(caret_);
    return removed;
}

} // namespace cind
