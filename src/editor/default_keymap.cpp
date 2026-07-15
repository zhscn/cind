#include "editor/default_keymap.hpp"

#include "editor/runtime.hpp"

#include <array>
#include <string_view>

namespace cind {

namespace {

struct Binding {
    std::string_view keys;
    std::string_view command;
};

constexpr std::array kBindings{
    Binding{"C-x C-c", "application.quit"},
    Binding{"C-x C-s", "file.save"},
    Binding{"C-x C-w", "file.save-as"},
    Binding{"C-x C-f", "file.open"},
    Binding{"C-x b", "buffer.switch"},
    Binding{"C-x k", "buffer.kill"},
    Binding{"C-x Right", "buffer.next"},
    Binding{"C-x Left", "buffer.previous"},
    Binding{"M-x", "command.palette"},
    Binding{"C-g", "keyboard.quit"},
    Binding{"C-s", "search.prompt"},
    Binding{"C-r", "search.backward-prompt"},
    Binding{"C-a", "cursor.line-start"},
    Binding{"C-e", "cursor.line-end"},
    Binding{"C-n", "cursor.next-line"},
    Binding{"C-p", "cursor.previous-line"},
    Binding{"C-f", "cursor.forward-character"},
    Binding{"C-b", "cursor.backward-character"},
    Binding{"C-v", "cursor.page-down"},
    Binding{"M-v", "cursor.page-up"},
    Binding{"C-/", "edit.undo"},
    Binding{"C-_", "edit.undo"},
    Binding{"C-x u", "edit.undo"},
    Binding{"C-M-/", "edit.redo"},
    Binding{"C-SPC", "selection.toggle-mark"},
    Binding{"C-w", "edit.kill-region"},
    Binding{"C-k", "edit.kill-line"},
    Binding{"M-w", "edit.copy-region"},
    Binding{"C-y", "edit.yank"},
    Binding{"C-d", "edit.delete-forward"},
    Binding{"C-l", "editor.redraw"},
    Binding{"C-M-f", "cursor.forward-expression"},
    Binding{"C-M-b", "cursor.backward-expression"},
    Binding{"C-M-u", "cursor.up-list"},
    Binding{"C-c e", "selection.expand"},
    Binding{"C-c s", "selection.contract"},
    Binding{"M-g g", "cursor.goto-line"},
    Binding{"M-%", "search.replace"},
    Binding{"C-h b", "help.keys"},
    Binding{"C-x =", "editor.position"},
    Binding{"RET", "edit.newline"},
    Binding{"TAB", "edit.indent"},
    Binding{"Backspace", "edit.delete-backward"},
    Binding{"Delete", "edit.delete-forward"},
    Binding{"C-u Backspace", "edit.delete-backward-raw"},
    Binding{"C-u Delete", "edit.delete-forward-raw"},
    Binding{"Left", "cursor.backward-character"},
    Binding{"Right", "cursor.forward-character"},
    Binding{"Up", "cursor.previous-line"},
    Binding{"Down", "cursor.next-line"},
    Binding{"PgUp", "cursor.page-up"},
    Binding{"PgDn", "cursor.page-down"},
    Binding{"Home", "cursor.line-start"},
    Binding{"End", "cursor.line-end"},
};

} // namespace

std::size_t bind_default_editor_keys(EditorRuntime& runtime, KeymapId keymap) {
    std::size_t count = 0;
    for (const Binding& binding : kBindings) {
        if (const std::optional<CommandId> command = runtime.commands().find(binding.command)) {
            runtime.keymaps().bind(keymap, binding.keys, *command);
            ++count;
        }
    }
    return count;
}

} // namespace cind
