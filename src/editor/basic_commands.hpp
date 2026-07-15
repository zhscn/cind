#pragma once

#include "cli/session.hpp"
#include "editor/command.hpp"

#include <functional>
#include <string>

namespace cind {

class EditorRuntime;

using EditSessionResolver = std::function<EditSession&(ViewId)>;

struct BasicEditorCommandHooks {
    std::function<int()> page_rows;
    std::function<void(std::string)> show_message;
    std::function<void()> edited;
    std::function<void()> caret_moved;
};

class BasicEditorCommands {
public:
    BasicEditorCommands(EditorRuntime& runtime, EditSessionResolver session,
                        BasicEditorCommandHooks hooks);

    void reset_preferred_column(ViewId view);

private:
    EditSession& session(ViewId view) const { return session_(view); }
    void move_horizontal(ViewId view, bool forward, const CommandInvocation& invocation);
    void move_vertical(ViewId view, int direction, bool page, const CommandInvocation& invocation);
    void move_line_boundary(ViewId view, bool end);
    void soft_delete(ViewId view, bool forward);
    void raw_delete(ViewId view, bool forward);
    void notify_edited();
    void notify_caret_moved();

    EditorRuntime* runtime_;
    EditSessionResolver session_;
    BasicEditorCommandHooks hooks_;
};

} // namespace cind
