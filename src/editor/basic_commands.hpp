#pragma once

#include "cli/session.hpp"
#include "editor/command.hpp"

#include <functional>
#include <string>

namespace cind {

class EditorRuntime;

struct BasicEditorCommandHooks {
    std::function<int()> page_rows;
    std::function<void(std::string)> show_message;
    std::function<void()> edited;
    std::function<void()> caret_moved;
};

class BasicEditorCommands {
public:
    BasicEditorCommands(EditorRuntime& runtime, EditSession& session,
                        BasicEditorCommandHooks hooks);

    void reset_preferred_column();

private:
    void move_horizontal(bool forward, const CommandInvocation& invocation);
    void move_vertical(int direction, bool page, const CommandInvocation& invocation);
    void move_line_boundary(bool end);
    void soft_delete(bool forward);
    void notify_edited();
    void notify_caret_moved();

    EditorRuntime* runtime_;
    EditSession* session_;
    BasicEditorCommandHooks hooks_;
};

} // namespace cind
