#pragma once

#include "editor/editor_application.hpp"
#include "formatting/cpp_indent_style.hpp"
#include "gui/editor_state.hpp"
#include "ui/editor_scene.hpp"
#include "ui/line_signs.hpp"
#include "ui/view_tree.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cind::gui {

class EditorModel {
public:
    EditorModel(std::string path, std::optional<std::string> initial, CppIndentStyle style,
                std::string style_origin, std::uint32_t initial_line,
                EditorPlatformServices platform_services = {},
                std::optional<std::string> init_file = std::nullopt);

    void layout_view(int rows, int columns, float visible_text_rows = 0.0F);
    ui::Scene compose(int rows, int columns, float visible_text_rows = 0.0F);
    bool handle_key(KeyStroke key, int page_rows);
    bool has_pending_key_sequence() const {
        return !application_.command_loop().pending_sequence().empty();
    }
    void insert_text(std::string_view text);
    void set_preedit(std::string_view text);
    void click(const ui::HitTarget& target);
    void scroll_lines(float delta);
    bool has_background_work() const { return application_.has_background_work(); }
    bool poll_background_work() { return application_.poll_background_work(); }
    bool should_quit() const { return application_.should_quit(); }
    void request_close(bool force = false) { (void)application_.request_close(force); }

    RevisionId revision() const { return application_.revision(); }
    EditorRenderState render_state();
    EditorStateSnapshot inspect();

private:
    struct SignCache {
        BufferId buffer;
        RevisionId revision = static_cast<RevisionId>(-1);
        std::uint32_t generation = static_cast<std::uint32_t>(-1);
        ui::LineSigns signs;
    };

    const ui::LineSigns& signs(WindowId window);
    static std::string pane_id(WindowId window);

    EditorApplication application_;
    std::deque<SignCache> sign_caches_;
    ui::ListViewport popup_viewport_;
    int last_rows_ = 24;
    std::string preedit_;
};

} // namespace cind::gui
