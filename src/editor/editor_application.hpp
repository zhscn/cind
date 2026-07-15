#pragma once

#include "cli/session.hpp"
#include "editor/basic_commands.hpp"
#include "editor/command_loop.hpp"
#include "editor/search_commands.hpp"
#include "formatting/cpp_indent_style.hpp"

#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace cind {

struct EditorApplicationSpec {
    std::string path;
    std::string initial_text;
    CppIndentStyle style;
    std::string style_origin;
    std::uint32_t initial_line = 0;
};

// Frontend-independent state and command controller for one editor
// application. Frontends translate native events into KeyStroke/text input
// and render the exposed document/view state; command semantics and
// application lifetime live here.
class EditorApplication {
public:
    explicit EditorApplication(EditorApplicationSpec spec);

    EditorRuntime& runtime() { return runtime_; }
    const EditorRuntime& runtime() const { return runtime_; }
    BufferId buffer_id() const { return buffer_id_; }
    ViewId view_id() const { return view_id_; }
    EditSession& session() { return session_; }
    const EditSession& session() const { return session_; }
    BasicEditorCommands& basic_commands() { return basic_commands_; }
    const BasicEditorCommands& basic_commands() const { return basic_commands_; }
    SearchCommands& search_commands() { return search_commands_; }
    const SearchCommands& search_commands() const { return search_commands_; }
    CommandLoop& command_loop() { return command_loop_; }
    const CommandLoop& command_loop() const { return command_loop_; }
    KeymapId default_keymap() const { return keymap_; }

    // Installs bindings for commands added after construction. Rebinding an
    // existing sequence updates it to the command currently registered under
    // the standard command name.
    void refresh_default_keymap();

    bool handle_key(KeyStroke key, int page_rows);
    void insert_text(std::string_view text);
    void reset_preferred_column() { basic_commands_.reset_preferred_column(); }

    RevisionId revision() const { return session_.snapshot().revision(); }
    bool dirty() const { return session_.buffer().modified(); }
    const std::string& path() const;
    const std::string& style_origin() const { return style_origin_; }
    std::uint32_t save_generation() const { return save_generation_; }

    const std::string& message() const { return message_; }
    std::string& message() { return message_; }
    void set_message(std::string message) { message_ = std::move(message); }
    const std::string& last_key() const { return last_key_; }
    const std::string& last_command() const { return last_command_; }

    bool reveal_caret() const { return reveal_caret_; }
    void show_caret() { reveal_caret_ = true; }
    void hide_caret() { reveal_caret_ = false; }

    bool has_background_work() const { return pending_save_.has_value(); }
    bool poll_background_work();
    bool should_quit() const { return quit_; }
    bool quit_armed() const { return quit_armed_; }
    void request_quit(bool force = false);

    // Records a save performed by an application command outside the common
    // file.save implementation, such as a frontend's current write-as flow.
    void mark_saved(Text content);

private:
    void register_commands();
    bool handle_loop_result(const CommandLoopResult& result);
    CommandContext command_context() { return CommandContext(runtime_, buffer_id_, view_id_); }
    void after_edit();
    void save();

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
    std::string message_;
    std::string last_key_;
    std::string last_command_;
    std::uint32_t save_generation_ = 0;
    bool reveal_caret_ = true;
    bool quit_armed_ = false;
    bool quit_ = false;

    struct PendingSave {
        Text content;
        std::future<std::error_code> result;
    };
    std::optional<PendingSave> pending_save_;
};

} // namespace cind
