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
    Binding{"C-q", "application.quit"},
    Binding{"C-S-q", "application.force-quit"},
    Binding{"C-s", "file.save"},
    Binding{"C-x C-s", "file.save"},
    Binding{"C-o", "file.save-as"},
    Binding{"C-f", "search.prompt"},
    Binding{"C-c", "editor.position"},
    Binding{"C-g", "keyboard.quit"},
    Binding{"C-a", "cursor.line-start"},
    Binding{"C-e", "cursor.line-end"},
    Binding{"C-n", "cursor.next-line"},
    Binding{"C-p", "cursor.previous-line"},
    Binding{"C-v", "cursor.page-down"},
    Binding{"C-z", "edit.undo"},
    Binding{"C-r", "edit.redo"},
    Binding{"C-SPC", "selection.toggle-mark"},
    Binding{"C-w", "edit.kill-region"},
    Binding{"C-k", "edit.kill-line"},
    Binding{"C-y", "edit.yank"},
    Binding{"C-l", "editor.redraw"},
    Binding{"M-f", "cursor.forward-expression"},
    Binding{"M-b", "cursor.backward-expression"},
    Binding{"M-u", "cursor.up-list"},
    Binding{"M-h", "selection.expand"},
    Binding{"M-j", "selection.contract"},
    Binding{"M-w", "edit.copy-region"},
    Binding{"M-g", "cursor.goto-line"},
    Binding{"M-n", "search.next"},
    Binding{"M-p", "search.previous"},
    Binding{"M-%", "search.replace"},
    Binding{"M-?", "help.keys"},
    Binding{"M-v", "cursor.page-up"},
    Binding{"RET", "edit.newline"},
    Binding{"TAB", "edit.indent"},
    Binding{"Backspace", "edit.delete-backward"},
    Binding{"Delete", "edit.delete-forward"},
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
