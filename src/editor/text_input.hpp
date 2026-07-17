#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cind {

// Single-line editable UTF-8 text owned by a focused UI control such as the
// minibuffer. The caret is a byte offset kept on an extended grapheme
// boundary; presentation converts it to cells or pixels.
class TextInput {
public:
    TextInput() = default;
    explicit TextInput(std::string text);

    const std::string& text() const { return text_; }
    std::size_t caret() const { return caret_; }

    bool insert(std::string_view text);
    bool erase_backward();
    bool erase_forward();
    bool move_backward();
    bool move_forward();
    bool move_word_backward();
    bool move_word_forward();
    bool move_to_start();
    bool move_to_end();
    std::optional<std::string> take_to_end();

private:
    std::string text_;
    std::size_t caret_ = 0;
};

} // namespace cind
