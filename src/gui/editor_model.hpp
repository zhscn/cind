#pragma once

#include "cli/session.hpp"
#include "editor/basic_commands.hpp"
#include "editor/command_loop.hpp"
#include "editor/search_commands.hpp"
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

class EditorModel {
public:
    EditorModel(std::string path, std::string initial, CppIndentStyle style,
                std::string style_origin, std::uint32_t initial_line);

    ui::Scene compose(int rows, int columns, float visible_text_rows = 0.0F);
    bool handle_key(KeyStroke key, int page_rows);
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
    const std::string& path() const;
    const ui::LineSigns& signs();
    void after_edit();
    void save();
    void register_commands();
    bool handle_loop_result(const CommandLoopResult& result);
    CommandContext command_context() { return CommandContext(runtime_, buffer_id_, view_id_); }

    EditorRuntime runtime_;
    BufferId buffer_id_;
    ViewId view_id_;
    EditSession session_;
    BasicEditorCommands basic_commands_;
    SearchCommands search_commands_;
    CommandLoop command_loop_;
    KeymapId keymap_;
    int command_page_rows_ = 1;
    std::string style_origin_;
    ui::LineSigns signs_;
    RevisionId sign_revision_ = static_cast<RevisionId>(-1);
    std::uint32_t save_generation_ = 0;
    std::uint32_t sign_generation_ = static_cast<std::uint32_t>(-1);
    int last_rows_ = 24;
    bool reveal_caret_ = true;
    std::string message_;
    std::string preedit_;
    std::string last_key_;
    std::string last_command_;
    bool quit_armed_ = false;
    bool quit_ = false;

    struct PendingSave {
        Text content;
        std::future<std::error_code> result;
    };
    std::optional<PendingSave> pending_save_;
};

} // namespace cind::gui
