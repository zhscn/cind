#pragma once

#include "cli/session.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "gui/editor_state.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"

#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace cind::gui {

enum class EditorKey : std::uint8_t {
    Unknown,
    A,
    E,
    N,
    P,
    Q,
    R,
    S,
    V,
    Z,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Backspace,
    Delete,
    Enter,
    Tab,
};

struct KeyModifiers {
    bool control = false;
    bool alt = false;
    bool shift = false;
};

class EditorModel {
public:
    EditorModel(std::string path, std::string initial, CppIndentStyle style,
                std::string style_origin, std::uint32_t initial_line);

    ui::Scene compose(int rows, int columns);
    bool handle_key(EditorKey key, KeyModifiers modifiers, int page_rows);
    void insert_text(std::string_view text);
    void set_preedit(std::string_view text);
    void click(ui::CellPoint point);
    void set_frame_rows(int rows) { last_rows_ = rows; }
    void scroll_lines(int delta);
    bool has_background_work() const { return pending_save_.has_value(); }
    bool poll_background_work();
    bool should_quit() const { return quit_; }
    void request_quit(bool force = false);

    RevisionId revision() const { return session_.snapshot().revision(); }
    EditorStateSnapshot inspect();

private:
    bool dirty() const;
    const ui::LineSigns& signs();
    void after_edit();
    void save();
    void move_horizontal(bool forward);
    void move_vertical(int delta);
    void move_home();
    void move_end();
    void erase_grapheme(bool forward);

    std::string path_;
    EditSession session_;
    std::string style_origin_;
    Text saved_text_;
    ui::EditorViewport viewport_;
    ui::LineSigns signs_;
    RevisionId sign_revision_ = static_cast<RevisionId>(-1);
    std::uint32_t save_generation_ = 0;
    std::uint32_t sign_generation_ = static_cast<std::uint32_t>(-1);
    int last_rows_ = 24;
    bool reveal_caret_ = true;
    std::string message_;
    std::string preedit_;
    std::string last_key_;
    bool quit_armed_ = false;
    bool quit_ = false;

    struct PendingSave {
        Text content;
        std::future<std::error_code> result;
    };
    std::optional<PendingSave> pending_save_;
};

} // namespace cind::gui
