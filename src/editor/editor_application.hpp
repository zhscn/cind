#pragma once

#include "cli/session.hpp"
#include "editor/basic_commands.hpp"
#include "editor/command_loop.hpp"
#include "editor/interaction.hpp"
#include "editor/search_commands.hpp"
#include "formatting/cpp_indent_style.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cind {

struct EditorApplicationSpec {
    std::string path;
    std::string initial_text;
    CppIndentStyle style;
    std::string style_origin;
    std::uint32_t initial_line = 0;
};

struct OpenBufferSnapshot {
    BufferId buffer;
    ViewId view;
    std::string name;
    std::optional<std::string> resource;
    bool modified = false;
    bool active = false;
    bool saving = false;
};

struct KeyBindingHint {
    std::string key;
    std::string command;
    bool prefix = false;
};

// Frontend-independent state and command controller for one editor
// application. Frontends translate native events into KeyStroke/text input
// and render the exposed document, interaction and view state.
class EditorApplication {
public:
    explicit EditorApplication(EditorApplicationSpec spec);

    EditorRuntime& runtime() { return runtime_; }
    const EditorRuntime& runtime() const { return runtime_; }
    BufferId buffer_id() const;
    ViewId view_id() const;
    EditSession& session();
    const EditSession& session() const;
    SearchCommands& search_commands() { return search_commands_; }
    const SearchCommands& search_commands() const { return search_commands_; }
    CommandLoop& command_loop() { return command_loop_; }
    const CommandLoop& command_loop() const { return command_loop_; }
    InteractionController& interaction() { return interaction_; }
    const InteractionController& interaction() const { return interaction_; }
    KeymapId default_keymap() const { return keymap_; }

    void refresh_default_keymap();

    bool handle_key(KeyStroke key, int page_rows);
    void insert_text(std::string_view text);
    void reset_preferred_column();

    std::expected<BufferId, std::string> open_file(std::string_view path);
    bool switch_buffer(BufferId buffer);
    std::expected<void, std::string> kill_buffer(BufferId buffer, bool force = false);
    std::vector<OpenBufferSnapshot> open_buffers() const;
    std::vector<KeyBindingHint> pending_key_hints() const;
    std::size_t buffer_count() const { return buffers_.size(); }

    RevisionId revision() const { return session().snapshot().revision(); }
    bool dirty() const { return session().buffer().modified(); }
    const std::string& path() const;
    const std::string& style_origin() const;
    std::uint32_t save_generation() const;

    const std::string& message() const { return message_; }
    std::string& message() { return message_; }
    void set_message(std::string message) { message_ = std::move(message); }
    const std::string& last_key() const { return last_key_; }
    const std::string& last_command() const { return last_command_; }

    bool reveal_caret() const { return reveal_caret_; }
    void show_caret() { reveal_caret_ = true; }
    void hide_caret() { reveal_caret_ = false; }

    bool has_background_work() const;
    bool poll_background_work();
    bool should_quit() const { return quit_; }
    bool quit_armed() const { return quit_armed_; }
    void request_quit(bool force = false);

    void mark_saved(Text content);

private:
    struct PendingSave {
        Text content;
        std::future<std::error_code> result;
    };

    struct BufferState {
        BufferId buffer;
        ViewId view;
        std::unique_ptr<EditSession> session;
        std::string style_origin;
        std::uint32_t save_generation = 0;
        std::optional<PendingSave> pending_save;
        std::vector<TextRange> selection_history;
    };

    BufferState& active_buffer();
    const BufferState& active_buffer() const;
    BufferState& state_for(BufferId buffer);
    const BufferState& state_for(BufferId buffer) const;
    EditSession& session_for(ViewId view);
    const EditSession& session_for(ViewId view) const;
    BufferId create_buffer(BufferSpec spec, CppIndentStyle style, std::string style_origin,
                           TextOffset caret = {});
    BufferId create_scratch_buffer();

    void register_commands();
    void register_interaction_providers();
    bool handle_loop_result(CommandLoopResult result);
    CommandContext command_context();
    void after_edit();
    void save();
    void mark_saved(BufferId buffer, Text content);
    void switch_relative(int delta);

    CommandResult begin_command_palette(CommandContext&, const CommandInvocation&) const;
    CommandResult accept_command_palette(CommandContext&, const CommandInvocation&);
    CommandResult begin_open_file(CommandContext&, const CommandInvocation&) const;
    CommandResult accept_open_file(CommandContext&, const CommandInvocation&);
    CommandResult begin_save_as(CommandContext&, const CommandInvocation&) const;
    CommandResult accept_save_as(CommandContext&, const CommandInvocation&);
    CommandResult begin_switch_buffer(CommandContext&, const CommandInvocation&) const;
    CommandResult accept_switch_buffer(CommandContext&, const CommandInvocation&);
    CommandResult begin_goto_line(CommandContext&, const CommandInvocation&) const;
    CommandResult accept_goto_line(CommandContext&, const CommandInvocation&);

    EditorRuntime runtime_;
    std::vector<std::unique_ptr<BufferState>> buffers_;
    std::size_t active_buffer_index_ = 0;
    InteractionController interaction_;
    BasicEditorCommands basic_commands_;
    SearchCommands search_commands_;
    CommandLoop command_loop_;
    KeymapId keymap_;
    CommandId command_palette_accept_;
    CommandId open_file_accept_;
    CommandId save_as_accept_;
    CommandId switch_buffer_accept_;
    CommandId help_keys_accept_;
    CommandId goto_line_accept_;
    int command_page_rows_ = 1;
    std::string message_;
    std::string last_key_;
    std::string last_command_;
    std::string kill_slot_;
    bool reveal_caret_ = true;
    bool quit_armed_ = false;
    bool quit_ = false;
};

} // namespace cind
