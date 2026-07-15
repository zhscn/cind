#include "editor/basic_commands.hpp"

#include "editor/runtime.hpp"
#include "ui/text_position.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace cind {

namespace {

int repeat_count(const CommandInvocation& invocation) {
    const std::int64_t count = invocation.repeat_count.value_or(1);
    return static_cast<int>(
        std::clamp<std::int64_t>(count, -static_cast<std::int64_t>(std::numeric_limits<int>::max()),
                                 static_cast<std::int64_t>(std::numeric_limits<int>::max())));
}

} // namespace

BasicEditorCommands::BasicEditorCommands(EditorRuntime& runtime, EditSessionResolver session,
                                         BasicEditorCommandHooks hooks)
    : runtime_(&runtime), session_(std::move(session)), hooks_(std::move(hooks)) {
    auto define = [this](std::string name, auto execute) {
        runtime_->commands().define(
            std::move(name),
            [execute = std::move(execute)](CommandContext& context,
                                           const CommandInvocation& invocation) mutable {
                execute(context.view_id(), invocation);
                return CommandResult{CommandCompleted{}};
            });
    };

    define("edit.undo", [this](ViewId view, const CommandInvocation&) {
        reset_preferred_column(view);
        const bool changed = session_(view).undo();
        if (changed) {
            notify_edited();
        }
        hooks_.show_message(changed ? "undo" : "nothing to undo");
    });
    define("edit.redo", [this](ViewId view, const CommandInvocation&) {
        reset_preferred_column(view);
        const bool changed = session_(view).redo();
        if (changed) {
            notify_edited();
        }
        hooks_.show_message(changed ? "redo" : "nothing to redo");
    });
    define("cursor.line-start",
           [this](ViewId view, const CommandInvocation&) { move_line_boundary(view, false); });
    define("cursor.line-end",
           [this](ViewId view, const CommandInvocation&) { move_line_boundary(view, true); });
    define("cursor.next-line", [this](ViewId view, const CommandInvocation& invocation) {
        move_vertical(view, 1, false, invocation);
    });
    define("cursor.previous-line", [this](ViewId view, const CommandInvocation& invocation) {
        move_vertical(view, -1, false, invocation);
    });
    define("cursor.page-down", [this](ViewId view, const CommandInvocation& invocation) {
        move_vertical(view, 1, true, invocation);
    });
    define("cursor.page-up", [this](ViewId view, const CommandInvocation& invocation) {
        move_vertical(view, -1, true, invocation);
    });
    define("cursor.forward-character", [this](ViewId view, const CommandInvocation& invocation) {
        move_horizontal(view, true, invocation);
    });
    define("cursor.backward-character", [this](ViewId view, const CommandInvocation& invocation) {
        move_horizontal(view, false, invocation);
    });
    define("edit.delete-backward", [this](ViewId view, const CommandInvocation&) {
        reset_preferred_column(view);
        const RevisionId revision = session_(view).snapshot().revision();
        soft_delete(view, false);
        if (session_(view).snapshot().revision() != revision) {
            notify_edited();
        }
    });
    define("edit.delete-forward", [this](ViewId view, const CommandInvocation&) {
        reset_preferred_column(view);
        const RevisionId revision = session_(view).snapshot().revision();
        soft_delete(view, true);
        if (session_(view).snapshot().revision() != revision) {
            notify_edited();
        }
    });
    define("edit.newline", [this](ViewId view, const CommandInvocation&) {
        reset_preferred_column(view);
        session_(view).enter();
        notify_edited();
    });
    define("edit.indent", [this](ViewId view, const CommandInvocation&) {
        reset_preferred_column(view);
        const RevisionId revision = session_(view).snapshot().revision();
        const IndentDecision decision = session_(view).indent();
        if (session_(view).snapshot().revision() != revision) {
            notify_edited();
        }
        hooks_.show_message(std::format("indent: {}", format_role_name(decision.role)));
    });
}

void BasicEditorCommands::reset_preferred_column(ViewId view) {
    session_(view).view().viewport().preferred_column.reset();
}

void BasicEditorCommands::move_horizontal(ViewId view, bool forward,
                                          const CommandInvocation& invocation) {
    EditSession& active = session_(view);
    reset_preferred_column(view);
    int count = repeat_count(invocation);
    if (count < 0) {
        forward = !forward;
        count = -count;
    }
    for (int index = 0; index < count; ++index) {
        const DocumentSnapshot snapshot = active.snapshot();
        active.set_caret(forward ? ui::next_grapheme(snapshot.content(), active.caret())
                                 : ui::previous_grapheme(snapshot.content(), active.caret()));
    }
    notify_caret_moved();
}

void BasicEditorCommands::move_vertical(ViewId view, int direction, bool page,
                                        const CommandInvocation& invocation) {
    EditSession& active = session_(view);
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    ViewportState& viewport = active.view().viewport();
    if (!viewport.preferred_column) {
        viewport.preferred_column =
            ui::display_column(text, active.caret(), active.style().tab_width);
    }
    const int page_scale = page && hooks_.page_rows ? std::max(1, hooks_.page_rows()) : 1;
    const std::int64_t requested = static_cast<std::int64_t>(direction) * page_scale *
                                   static_cast<std::int64_t>(repeat_count(invocation));
    const int delta = static_cast<int>(std::clamp<std::int64_t>(
        requested, -static_cast<std::int64_t>(std::numeric_limits<int>::max()),
        static_cast<std::int64_t>(std::numeric_limits<int>::max())));
    const int current_line = static_cast<int>(text.position(active.caret()).line);
    const int last_line = static_cast<int>(text.line_count()) - 1;
    const int target = std::clamp(current_line + delta, 0, last_line);
    active.set_caret(ui::offset_at_display_column(
        text, {.line = static_cast<std::uint32_t>(target), .column = *viewport.preferred_column},
        active.style().tab_width));
    notify_caret_moved();
}

void BasicEditorCommands::move_line_boundary(ViewId view, bool end) {
    EditSession& active = session_(view);
    reset_preferred_column(view);
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    const std::uint32_t line = text.position(active.caret()).line;
    active.set_caret(end ? text.line_content_end(line) : text.line_start(line));
    notify_caret_moved();
}

void BasicEditorCommands::soft_delete(ViewId view, bool forward) {
    EditSession& active = session_(view);
    const DocumentSnapshot snapshot = active.snapshot();
    const Text& text = snapshot.content();
    const TextOffset caret = active.caret();
    if ((forward && caret.value >= text.size_bytes()) || (!forward && caret.value == 0)) {
        return;
    }
    const TextOffset target = forward ? caret : ui::previous_grapheme(text, caret);
    const char character = text.byte_at(target);
    auto is_open = [](char ch) { return ch == '(' || ch == '[' || ch == '{'; };
    auto is_close = [](char ch) { return ch == ')' || ch == ']' || ch == '}'; };
    auto partner = [](char ch) {
        switch (ch) {
        case '(':
            return ')';
        case '[':
            return ']';
        case '{':
            return '}';
        case ')':
            return '(';
        case ']':
            return '[';
        default:
            return '{';
        }
    };
    if (is_open(character) || is_close(character)) {
        if (is_open(character) && target.value + 1 < text.size_bytes() &&
            text.byte_at(TextOffset{target.value + 1}) == partner(character)) {
            active.erase(TextRange{target, TextOffset{target.value + 2}});
            return;
        }
        if (is_close(character) && target.value >= 1 &&
            text.byte_at(TextOffset{target.value - 1}) == partner(character)) {
            active.erase(TextRange{TextOffset{target.value - 1}, TextOffset{target.value + 1}});
            return;
        }
        active.set_caret(forward ? TextOffset{target.value + 1} : target);
        hooks_.show_message("soft delete: pair not empty (moved over)");
        notify_caret_moved();
        return;
    }
    if (character == '"' || character == '\'') {
        const char other =
            forward
                ? (target.value >= 1 ? text.byte_at(TextOffset{target.value - 1}) : '\0')
                : (target.value + 1 < text.size_bytes() ? text.byte_at(TextOffset{target.value + 1})
                                                        : '\0');
        if (other == character) {
            const std::uint32_t start = forward ? target.value - 1 : target.value;
            active.erase(make_range(start, start + 2));
            return;
        }
        active.set_caret(forward ? TextOffset{target.value + 1} : target);
        hooks_.show_message("soft delete: literal not empty (moved over)");
        notify_caret_moved();
        return;
    }
    active.erase(forward ? TextRange{caret, ui::next_grapheme(text, caret)}
                         : TextRange{ui::previous_grapheme(text, caret), caret});
}

void BasicEditorCommands::notify_edited() {
    if (hooks_.edited) {
        hooks_.edited();
    }
}

void BasicEditorCommands::notify_caret_moved() {
    if (hooks_.caret_moved) {
        hooks_.caret_moved();
    }
}

} // namespace cind
