# Command and interaction architecture

cind uses one editor command pipeline for terminal and graphical presentation. Platform adapters
normalize input and render the shared scene; editing behavior lives in named commands over explicit
runtime, buffer, and view state.

```text
terminal bytes ─┐                    active Window/View/Buffer/Mode
                ├─> KeyStroke ───────────────┬─> scoped Keymap stack ─> CommandLoop
SDL key events ─┘                            │                              │
                                             │                              └─> CommandRegistry
terminal UTF-8 ─┐                            │                                      │
SDL text/IME ───┴─> focused text target ─────┘                                      ├─> EditSession
                         │                                                           ├─> runtime state
                         └────────────> InteractionController <──────────────────────└─> InteractionRequest
                                              │
                                              └─> named candidate provider

Editor state ─> ui::Scene ─┬─> ANSI renderer
                           └─> Skia renderer
```

`EditorRuntime` owns command, keymap, input-state, interaction-provider, buffer, view, and window
registries. A `Buffer` owns text, revision history, modes, buffer-local settings, and buffer-local
keymaps. A `View` refers to one buffer and owns caret, selection, viewport, input-state stack,
view-local settings, and view-local keymaps. A `Window` is a focus and display target that binds one
View and may contribute window-local keymaps. Multiple Views can refer to the same Buffer without
sharing display state.

`EditorApplication` owns buffer save state and the EditSession associated with each window-buffer
view. Switching buffers changes the active Window's View binding; returning to a buffer restores
that Window's cached View. Editing style is shared at buffer scope, so separate Views of a Buffer
use the same language-editing policy.

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

A keymap is a named trie from key sequences to command IDs and may inherit one explicitly assigned
parent keymap. A trie entry is either a complete command, an inline prefix, or a reference to
another named keymap used as a prefix map. Writing an entry replaces the previous command or prefix
at that sequence; extending a command replaces it with an inline prefix. Child bindings override
exact parent bindings while unbound branches inherit parent behavior. Parent and prefix-map
relationship cycles are rejected.

A keymap may remap one command ID to another without rebinding its key sequences. Layered lookup
first resolves a command or prefix, then scans the same active layers once for a remap. The
replacement is not remapped recursively. This keeps minor-mode command substitution separate from
key placement and makes the complete layered resolver a side-effect-free query shared by dispatch
and scripting.

The focused target supplies an ordered keymap stack from most specific to least specific. Each View
may own a stack of registered input states. The bottom element is its durable state and elements
above it are transient states. Transient state keymaps are inserted from stack top to bottom ahead
of the durable state's maps, followed by Window, View, Buffer, enabled minor modes in reverse
activation order, major mode, `editor.default`, and `application.global`. Duplicate map identities
are kept only at their highest-priority occurrence. Views displaying the same Buffer have
independent state stacks.

Push, pop and base replacement publish typed state-change events containing the View and the
previous and next state identities. Resetting a stack emits one pop event per transient state and
preserves the base. An input state may also provide a key handler. The focused View's top handler
runs before layered lookup and returns pass, consume, dispatch-command or an error. Passing refreshes
the state layers and continues through the command loop. Consuming and errors clear pending chords;
dispatch-command enters the same checked command execution path as keymap lookup.

An interaction uses its local map followed by `application.global`; picker maps inherit the common
interaction text map. Window, View, state, Buffer, and mode maps belonging to the obscured document
are not active while the interaction owns focus, and document input-state handlers are bypassed, so
an unbound editing key cannot mutate the document behind a popup.

Lookup evaluates the complete pending sequence against every active layer on each keystroke. The
first layer that recognizes that complete sequence decides whether it is a command or a prefix. A
sparse prefix in a high-priority map therefore contributes its own continuations without hiding
different continuations in lower maps. A command or prefix defined by the higher map for the same
complete sequence still takes precedence.

`application.global` contains bindings that are valid independently of the focused text target,
including `C-x C-c`. Document editing and buffer commands remain in `editor.default`; interaction
editing remains in the interaction keymap family.

Always-active override maps are resolved before a pending sequence. The built-in system override
binds `C-g` to `keyboard.quit`, allowing it to cancel a prefix or focused interaction through the
same command path. Override maps contain complete bindings rather than prefix trees.

The default keymap follows Emacs conventions:

- `C-a`, `C-e`, `C-n`, `C-p`, `C-f`, `C-b`, `C-v`, and `M-v` move the caret or viewport;
- `C-s` and `C-r` start forward and backward search;
- `C-/`, `C-_`, and `C-x u` undo, while `C-M-/` redoes;
- `C-x C-f`, `C-x C-s`, `C-x C-w`, and `C-x C-c` open, save, write, and quit;
- `C-x b`, `C-x k`, `C-x Left`, and `C-x Right` manage the active buffer;
- `M-x` opens the command palette and `C-h b` opens searchable key-binding help;
- `C-M-f`, `C-M-b`, and `C-M-u` perform structural movement;
- `M-g n`, `M-g p`, and `` C-x ` `` navigate the current location list across source buffers;
- `C-c e` and `C-c s` expand and contract the structural selection.

The bundled Guile module `(cind core)` owns these default binding tables. Its installer binds only
commands present in the application composition, so optional capabilities can join the shared TUI
and GUI keymap without duplicating policy in C++. The keymap registry and precedence rules remain
C++ mechanisms.

## Command loop and prefix help

One `CommandLoop` follows the application's active input focus. It owns the ordered scoped keymap
stack, always-active override maps, a pending multi-key sequence, its highest-precedence matching
keymap, and an optional repeat count. Dispatch returns a structured status:
not handled, prefix, executed, awaiting input, disabled, cancelled, or error.

An undefined key after a recognized prefix consumes the sequence and clears pending and repeat
state. A single unbound key remains available to the platform text-input path. `C-g` invokes
`keyboard.quit` from the system override map.

The command loop merges immediate continuations across active keymaps and their parents using the
same precedence as dispatch. While a sequence is pending, the scene contains a popup listing the
next keys and their command names. This is the which-key view. Runtime, mode, inherited, and user
bindings appear without a separate help table.

Commands receive `CommandContext`, which names the runtime, window, buffer, and view explicitly.
Settings resolution follows the same context. Command callbacks do not depend on process-global
editor state.

## Interaction protocol

A command returns `CommandCompleted`, `InteractionRequest`, or a named `CommandDispatch`. A request
describes a text prompt or candidate picker using prompt text, initial input, a history name, a
candidate-provider name, an accept command ID, and typed arguments. The command loop returns the request to
`EditorApplication`; `InteractionController` owns the active state while the frontend continues its
normal event loop.

An interaction owns a single-line UTF-8 `TextInput` with a caret on an extended grapheme boundary.
Text events insert at the caret and refresh candidates. The common interaction keymap binds
`C-f`/`C-b`, Right/Left, `C-a`/`C-e`, Home/End, Backspace, `C-d`/Delete, Enter, and Escape to editing,
submission, and cancellation commands. The picker child map adds `C-n`/Down/Tab and `C-p`/Up for
candidate navigation. Submission returns a named command dispatch that the command loop follows,
so the accepted command remains visible as the executed command. An accept command may return
another request, which supports multi-step interactions without retaining a C++ closure.

Candidate providers return semantic values, labels, details, and filter text either immediately or
through a cancellable worker job. Provider preparation runs on the editor thread and captures
immutable worker input; filesystem traversal and large candidate ranking run through the async
runtime. Every input change receives a monotonically increasing generation, and only results for
the active generation can update the interaction. Command, key-binding, open-buffer, and filesystem
providers implement the command palette, key help, buffer switching, and file opening. GUI and TUI
render the same candidate and loading state through frontend-specific layout. The TUI places the
prompt in the echo area and candidates in a cell popup. The GUI combines an interactive picker
prompt and its candidates in an elevated logical-pixel overlay; prefix help uses the same structured
popup content in a bottom-aligned overlay.

## Extension boundary

Command names, keymap names, IDs, `SettingValue` arguments, `CommandContext`, interaction requests,
and named providers form the Guile host boundary. `CommandContext` explicitly identifies the
Window, View, Buffer, Project, and Runtime involved in an invocation. Scheme receives explicit host
capabilities instead of frontend objects or a process-global current editor. Definitions are sealed
after configuration; window, buffer, view, project, and application values remain mutable through
explicit runtime APIs. [Guile scripting architecture](scripting.md) defines interpreter ownership,
module loading, error containment, and the development-service boundary.

## Inspection

The GUI inspector exposes `editor.command_loop`, `editor.scripting`, `editor.interaction`, `editor.buffers`,
`editor.windows`, `editor.location_navigation`, and `editor.focus`. Command-loop state includes
keymap names with their scopes and parent chains, override maps, pending keys, the highest matching
keymap, repeat count, and last command. Interaction state includes prompt kind, input caret,
provider, selection, generation, errors, and candidates. Buffer state includes resource and
lifecycle data; Window state identifies each Window's bound View and Buffer.
The popup is also represented as `scene.region.popup`, including structured title, input, visible
item window, global item count and selection alongside its cell geometry, surface, and overlay
anchor. Each frontend projects this semantic content into its native layout.
