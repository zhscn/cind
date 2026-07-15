# Command and interaction architecture

cind uses one editor command pipeline for terminal and graphical presentation. Platform adapters
normalize input and render the shared scene; editing behavior lives in named commands over explicit
runtime, buffer, and view state.

```text
terminal bytes ─┐
                ├─> KeyStroke ─> layered Keymap ─> CommandLoop ─> CommandRegistry
SDL key events ─┘                                      │                 │
                                                       │                 ├─> EditSession
terminal UTF-8 ─┐                                      │                 ├─> buffer/view/runtime
SDL text/IME ───┴─> active text target ────────────────┘                 └─> InteractionRequest
                         │                                                  │
                         └────────────> InteractionController <─────────────┘
                                              │
                                              └─> named candidate provider

Editor state ─> ui::Scene ─┬─> ANSI renderer
                           └─> Skia renderer
```

`EditorRuntime` owns command, keymap, and interaction-provider registries. Each open buffer has its
own `View` and `EditSession`, including caret, selection, viewport, undo history, incremental syntax
state, style, and save state. `EditorApplication` owns the open-buffer list and selects the active
buffer/view context for each command invocation.

## Normalized input

`KeyStroke` represents a character or named key together with Control, Alt, Shift, and Super
modifiers. Keymap notation uses forms such as `C-x C-s`, `C-M-f`, `M-v`, `RET`, `TAB`, `PgDn`, and
`Backspace`. The parser and formatter provide the configuration and inspection representation.

Printable text is a separate input channel. SDL text and IME events and terminal UTF-8 input go to
the active text target directly. Modified keys and keys that continue a pending sequence go through
the command loop. A platform adapter consumes the text event corresponding to a successfully
handled printable sequence continuation, so a key such as the `b` in `C-x b` is not inserted after
the command executes.

## Keymaps and default bindings

A keymap is a named trie from key sequences to command IDs. A trie node is either a complete command
or a prefix; binding a command where a prefix exists, or extending an existing command binding,
fails during configuration. An exact binding may be replaced before the registry is sealed.

Each input focus supplies an ordered keymap stack from most specific to least specific. The first
layer that recognizes the current sequence owns it. A prefix in a more specific layer masks a
complete binding in a less specific layer. This supports mode, transient, interaction, and user
overrides without keymap inheritance or dynamically bound ambient state.

The default keymap follows Emacs conventions:

- `C-a`, `C-e`, `C-n`, `C-p`, `C-f`, `C-b`, `C-v`, and `M-v` move the caret or viewport;
- `C-s` and `C-r` start forward and backward search;
- `C-/`, `C-_`, and `C-x u` undo, while `C-M-/` redoes;
- `C-x C-f`, `C-x C-s`, `C-x C-w`, and `C-x C-c` open, save, write, and quit;
- `C-x b`, `C-x k`, `C-x Left`, and `C-x Right` manage the active buffer;
- `M-x` opens the command palette and `C-h b` opens searchable key-binding help;
- `C-M-f`, `C-M-b`, and `C-M-u` perform structural movement;
- `C-c e` and `C-c s` expand and contract the structural selection.

`bind_default_editor_keys` binds only commands registered by the application composition. Optional
capabilities can therefore join the shared TUI and GUI keymap without duplicating binding tables.

## Command loop and prefix help

One `CommandLoop` belongs to one input focus. It owns the ordered keymap stack, a pending multi-key
sequence, its owning keymap, and an optional repeat count. Dispatch returns a structured status:
not handled, prefix, executed, awaiting input, disabled, cancelled, or error.

An undefined key after a recognized prefix consumes the sequence and clears pending and repeat
state. A single unbound key remains available to the platform text-input path. `C-g` cancels either
a pending sequence or the active interaction.

The keymap registry enumerates the immediate continuations of any trie prefix. While a sequence is
pending, the scene contains a popup listing the next keys and their command names. This is the
which-key view. It derives its contents from the active keymap, so runtime and user bindings appear
without a separate help table.

Commands receive `CommandContext`, which names the runtime, buffer, and view explicitly. Settings
resolution follows the same context. Command callbacks do not depend on process-global editor
state.

## Interaction protocol

A command returns either `CommandCompleted` or `InteractionRequest`. A request describes a text
prompt or candidate picker using prompt text, initial input, a history name, a candidate-provider
name, an accept command ID, and typed arguments. The command loop returns the request to
`EditorApplication`; `InteractionController` owns the active state while the frontend continues its
normal event loop.

Text input appends to the interaction input. Backspace removes one UTF-8 code point. Up, Down, and
Tab navigate picker candidates. Enter invokes the accept command with the selected candidate or
submitted string appended to its arguments. Escape and `C-g` cancel. An accept command may return
another request, which supports multi-step interactions without retaining a C++ closure.

Candidate providers return semantic values, labels, details, and filter text. The controller applies
case-insensitive, whitespace-separated orderless filtering and stable ranking. Command, key-binding,
open-buffer, and filesystem providers implement the command palette, key help, buffer switching,
and file opening. GUI and TUI render the same candidate state as a fixed popup over the editor grid.

## Extension boundary

Command names, keymap names, IDs, `SettingValue` arguments, `CommandContext`, interaction requests,
and named providers form the scripting host boundary. An extension language can define commands,
bind sequences, and register providers during startup without receiving pointers to frontend or C++
model objects. Definitions are sealed after configuration; buffer, view, project, and application
values remain mutable through explicit runtime APIs.

The host contract does not depend on a particular scripting language. A language binding translates
its callable value to a registry command and translates command actions back to the data structures
above. Interpreter lifetime, garbage collection, interruption, and capability policy remain outside
the input and editor-state model.

## Inspection

The GUI inspector exposes `editor.command_loop`, `editor.interaction`, and `editor.buffers`.
Command-loop state includes active keymaps, pending keys, the owning keymap, repeat count, and last
command. Interaction state includes prompt kind, input, provider, selection, generation, errors, and
candidates. Buffer state includes buffer/view IDs, names, resources, modified and saving flags, and
the active buffer. The popup is also represented as `scene.region.popup`, including its primitives,
selection, geometry, surface, and overlay anchor.
