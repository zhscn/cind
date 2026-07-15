# Command and input architecture

cind uses one editor command pipeline for terminal and graphical presentation. Platform adapters
normalize input and render the shared scene; editing behavior lives in named commands over explicit
runtime, buffer, and view state.

```text
terminal bytes ─┐
                ├─> KeyStroke ─> layered Keymap ─> CommandLoop ─> CommandRegistry
SDL key events ─┘                                      │                 │
                                                       │                 ├─> EditSession
terminal UTF-8 ─┐                                      │                 ├─> buffer/view/runtime
SDL text/IME ───┴─> text input ────────────────────────┘                 └─> MinibufferRequest

Editor state ─> ui::Scene ─┬─> ANSI renderer
                           └─> Skia renderer
```

`EditorRuntime` owns command and keymap registries. `EditSession` owns the incremental analysis and
undo-aware caret behavior for one buffer/view pair. `BasicEditorCommands` registers navigation,
newline, indentation, grapheme deletion, balanced soft deletion, undo, and redo once for both
frontends. `SearchCommands` owns the search query and registers the same non-blocking search flow
for both frontends. Window, file-dialog, clipboard, and process-lifecycle commands are platform
services registered under stable command names.

## Normalized input

`KeyStroke` represents a character or a named key together with Control, Alt, Shift, and Super
modifiers. Keymap notation uses forms such as `C-x C-s`, `M-v`, `C-S-q`, `RET`, `TAB`, `PgDn`, and
`Backspace`. The parser and formatter provide the configuration and inspection representation.

Printable text is a separate input channel. SDL text and IME events and terminal UTF-8 input go to
the active text target directly. This preserves composed Unicode input and prevents keyboard-layout
details from leaking into editing commands. Modified and named keys go through the command loop.

## Keymaps and layers

A keymap is a named trie from key sequences to command IDs. A trie node is either a complete command
or a prefix; binding a command where a prefix exists, or extending an existing command binding,
fails during configuration. An exact binding may be replaced before the registry is sealed.

Each input focus supplies an ordered keymap stack from most specific to least specific. The first
layer that recognizes the current sequence owns it. A prefix in a more specific layer therefore
masks a complete binding in a less specific layer. This makes mode, transient, minibuffer, and user
overrides explicit without keymap inheritance or dynamically bound ambient state.

`bind_default_editor_keys` is the common default table. It binds commands that are registered in the
application composition, so optional capability providers can add commands without duplicating the
TUI and GUI binding tables.

## Command loop

One `CommandLoop` belongs to one input focus. It owns:

- the active ordered keymap stack;
- a pending multi-key sequence;
- an optional repeat count forwarded through `CommandInvocation`;
- the active minibuffer request and input.

Dispatch returns a structured status: not handled, prefix, executed, awaiting input, disabled,
cancelled, or error. Frontends use the status to request redraw and display command feedback. An
undefined key after a recognized prefix consumes the full sequence and clears pending and repeat
state. A single unbound key remains available to the platform text-input path.

Commands receive `CommandContext`, which names the runtime, buffer, and view explicitly. Settings
resolution follows the same explicit context. Command callbacks do not depend on process-global
editor state.

## Minibuffer protocol

A command returns either `CommandCompleted` or `MinibufferRequest`. A request contains prompt text,
initial input, history and completion-provider names, an accept command ID, and typed arguments.
The command loop stores this data and remains non-blocking while the frontend continues its normal
event loop.

Text input appends to the minibuffer input. Backspace removes one UTF-8 code point, Enter invokes the
accept command with the submitted string appended to its arguments, and Escape or `C-g` cancels.
The accept command may return another request, which supports multi-step interactions without a
retained C++ closure. History storage and completion providers are named services and can evolve
independently of the prompt state machine.

## Extension boundary

Command names, keymap names, IDs, `SettingValue` arguments, `CommandContext`, and command actions are
the scripting host boundary. An extension language can define commands, bind sequences, and register
history or completion providers during startup without receiving pointers to frontend or C++ model
objects. Definitions are sealed after configuration; buffer, view, project, and application values
remain mutable through their explicit runtime APIs.

The host contract does not depend on a particular scripting language. A language binding translates
its callable value to a registry command and translates command actions back to the data structures
above. This keeps interpreter lifetime, garbage collection, interruption, and capability policy out
of the input and editor-state model.

## Inspection

The GUI inspector exposes `editor.command_loop`, including active keymaps, pending keys, the keymap
that owns a pending prefix, repeat count, last command, and minibuffer prompt/input/provider state.
This connects a platform input event to key resolution and command execution when editing behavior
is being debugged.
