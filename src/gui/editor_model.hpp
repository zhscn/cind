#pragma once

#include "editor/editor_application.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "gui/editor_state.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace cind::gui {

class EditorModel {
public:
    EditorModel(std::string path, std::string initial, CppIndentStyle style,
                std::string style_origin, std::uint32_t initial_line);

    ui::Scene compose(int rows, int columns, float visible_text_rows = 0.0F);
    bool handle_key(KeyStroke key, int page_rows);
    bool has_pending_key_sequence() const {
        return !application_.command_loop().pending_sequence().empty();
    }
    void insert_text(std::string_view text);
    void set_preedit(std::string_view text);
    void click(ui::CellPoint point);
    void set_frame_rows(int rows) { last_rows_ = rows; }
    void scroll_lines(int delta);
    bool has_background_work() const { return application_.has_background_work(); }
    bool poll_background_work() { return application_.poll_background_work(); }
    bool should_quit() const { return application_.should_quit(); }
    void request_quit(bool force = false) { application_.request_quit(force); }

    RevisionId revision() const { return application_.revision(); }
    EditorStateSnapshot inspect();

private:
    const ui::LineSigns& signs();

    EditorApplication application_;
    ui::LineSigns signs_;
    BufferId sign_buffer_;
    RevisionId sign_revision_ = static_cast<RevisionId>(-1);
    std::uint32_t sign_generation_ = static_cast<std::uint32_t>(-1);
    int last_rows_ = 24;
    std::string preedit_;
};

} // namespace cind::gui
