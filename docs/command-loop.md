# Command and interaction architecture

cind uses one editor command pipeline for terminal and graphical presentation. Platform adapters
normalize input and render the shared scene; editing behavior lives in named commands over explicit
runtime, buffer, and view state.

```text
terminal bytes ─┐                    focused Window/View/Buffer/Mode
                ├─> KeyStroke ───────────────┬─> scoped Keymap stack ─> CommandLoop
SDL key events ─┘                            │                              │
                                             │                              └─> CommandRegistry
terminal UTF-8 ─┐                            │                                      │
SDL text/IME ───┴─> focused text target ─────┘                                      ├─> EditSession
                         │                                                           ├─> runtime state
                         └────────────> transient minibuffer View <──────────────────└─> InteractionRequest
                                              │
                                              ├─> InteractionController
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

A View selection is a directional, anchor-backed list of ranges with one primary range. Each range
records character, line, block, or syntax-node granularity. Strategy-owned Scheme metadata carries
selection semantics without adding policy to the editor core. The primary head is the View caret;
moving it preserves the other ranges. Document transactions settle all endpoints through edits,
and a window split copies the full selection model into the new View. Scene composition highlights
every non-empty range in both frontends.

Selection lifecycle is part of command completion. A completed command may preserve, collapse, or
replace the full Selection; the default result asks
the active InputStrategy to choose `collapse` or `preserve` when the command chain changed its
context Buffer. Direct text and IME commits use that same strategy policy. Document edit hooks
invalidate edit-dependent caches and request presentation updates. Native transactions and
multi-step scripted verbs therefore retain anchor-backed selections between edits.

## Normalized input

`KeyStroke` represents a character or named key together with Control, Alt, Shift, and Super
modifiers. Keymap notation uses forms such as `C-x C-s`, `C-M-f`, `M-v`, `RET`, `TAB`, `PgDn`, and
`Backspace`. The parser and formatter provide the configuration and inspection representation.

Printable text has a separate commit channel, but its keydown always enters the command loop first.
SDL text and IME commits are discarded when the corresponding keydown was consumed. Otherwise the
UTF-8 commit enters `EditorApplication::insert_text`, which applies the focused InputState's text
policy. TUI characters follow the same ordering within their single decoded input event.

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

The focused target supplies raw keymap sources to `(cind command)`, which assembles an ordered stack
from most specific to least specific. Each View may own a stack of registered input states. The
bottom element is its durable state and elements above it are transient states. The default policy
inserts transient state keymaps from stack top to bottom ahead of the durable state's maps, followed
by Window, View, Buffer, enabled minor modes in reverse activation order, major mode,
`editor.default`, and `application.global`. Duplicate map identities are kept only at their
highest-priority occurrence. Views displaying the same Buffer have independent state stacks.

An application can replace its editor, application, and always-active override roots with
`configure-keymap-policy!`. C++ exposes the focused Buffer kind and the named keymaps attached to
each source, validates the assembled result, and passes keymap IDs and diagnostic scopes to
`CommandLoop`. Root selection and layer precedence are Scheme policy; trie lookup, remapping,
pending sequences, and command execution remain native mechanisms.

Major and minor modes form explicit same-kind parent hierarchies. A child mode inherits settings,
interaction properties, semantic-to-definition thing bindings, and the nearest ancestor keymap.
When both child and parent
have keymaps, the child's primary keymap receives the parent's primary map as its default keymap
parent. Active minor modes are evaluated in reverse activation order before the major mode.

Each mode may declare an `editing` or `interface` interaction class, an initial InputState override,
and named thing bindings. Named input strategies map both interaction classes to durable states.
Each View may select its own strategy or inherit the application default. A mode-specific initial
state takes precedence over that mapping; otherwise the effective interaction class selects the
durable state. Mode policy changes publish the Buffer identity and before/after policies, then
changes to the effective interaction class or initial state rederive the base state of every View
displaying that Buffer through its selected strategy while preserving each View's transient state
stack. Semantic thing changes leave the View's input posture intact. This makes a
minor mode that changes an interface into editable text sufficient to change modal behavior without
package-specific key-routing code.

Push, pop and base replacement publish typed state-change events containing the View and the
previous and next state identities. Resetting a stack emits one pop event per transient state and
preserves the base. Each state may own `on-enter` and `on-exit` policy over its stack-membership
lifetime. Pushing a second transient state obscures the first without ending that lifetime; pop,
base replacement, and View release perform state-local cleanup. Destroying a View resets its
transients and exits its durable state before releasing the View. Lifecycle failures are contained
as scripting diagnostics after the authoritative transition. An input state may also provide a key handler. The focused View's top handler
runs after the always-active override layer and before ordinary layered lookup. It returns pass,
consume, dispatch-command, pending feedback, or an error. Passing refreshes the state layers and
continues through the command loop. Consuming and errors clear pending chords; dispatch-command
enters the same checked command execution path as keymap lookup. Pending feedback belongs to the
View's current state, consumes the key, and supplies a display sequence plus structured hints to
both frontends. State transitions invalidate feedback automatically.

Every normalized keystroke enters this dispatch path, including printable characters. A consumed
character ends at the command loop; a graphical frontend discards its paired text-input event. An
unconsumed character may subsequently arrive as UTF-8 text, and `EditorApplication::insert_text`
consults the focused input state's `accept` or `ignore` text policy before editing. TUI input applies
the same contract in one step by inserting the decoded character only after dispatch leaves it
unconsumed. Text input remains accepted while a minibuffer owns focus, independently of the
obscured document's state.

An interaction creates a transient minibuffer Buffer, View, and Window. Its View carries an
interaction keymap that inherits `editor.text-input`; picker maps override only candidate
navigation. The minibuffer uses the same durable InputState and handler pipeline as a document
View, followed by its Window, View, and Buffer layers and `application.global`. `editor.default`
and the obscured document's Window, View, Buffer, mode, and transient state layers are absent from
this focus stack. Ordinary movement, deletion, undo, kill, yank, selection, and prefix commands
therefore operate on the minibuffer through their normal `CommandContext` without interaction-only
command copies.

The focused View state also supplies its cursor shape and modeline indicator. These presentation
properties travel through the shared Scene and are rendered by both terminal and graphical
frontends. A state may additionally supply pure document-position hints derived from the current
document revision, Selection, and effective mode policy. The application validates and memoizes the
derived labels, while Scene composition maps byte offsets to visible replacement spans; neither the
ANSI nor Skia renderer calls scripting policy. Popup and echo-area composition render the
minibuffer View's caret as a beam cursor.

Lookup evaluates the complete pending sequence against every active layer on each keystroke. The
first layer that recognizes that complete sequence decides whether it is a command or a prefix. A
sparse prefix in a high-priority map therefore contributes its own continuations without hiding
different continuations in lower maps. A command or prefix defined by the higher map for the same
complete sequence still takes precedence.

`application.global` contains bindings that are valid independently of the focused text target,
including `C-x C-c`. `editor.text-input` contains bindings whose command semantics apply to any
focused editable text. `editor.default` inherits that map and adds document and buffer commands.
Minibuffer interaction maps inherit the text commands directly without exposing document-local
maps.

Always-active override maps are resolved before a pending sequence. The built-in system override
binds `C-g` to `keyboard.quit`, allowing it to cancel a prefix, focused interaction, or transient
handler state through the same command path. Override maps contain complete bindings rather than
prefix trees.

The default keymap follows Emacs conventions:

- `C-a`, `C-e`, `C-n`, `C-p`, `C-f`, `C-b`, `C-v`, and `M-v` move the caret or viewport;
- `C-s` and `C-r` start forward and backward search;
- `C-/`, `C-_`, and `C-x u` undo, while `C-M-/` redoes;
- `C-x C-f`, `C-x C-s`, `C-x C-w`, and `C-x C-c` open, save, write, and quit;
- `C-x b`, `C-x k`, `C-x Left`, and `C-x Right` manage the active buffer;
- `M-x` opens the command palette and `M-:` evaluates a Scheme expression; `C-h b`, `C-h k`,
  `C-h x`, `C-h m`, `C-h f`, and `C-h v`
  describe active bindings, keys, commands, modes, Scheme functions, and Scheme variables;
- `C-M-f`, `C-M-b`, and `C-M-u` perform structural movement;
- `M-g n`, `M-g p`, and `` C-x ` `` navigate the current location list across source buffers;
- `C-c e` enters sticky structural selection and expands once; `C-c s` contracts its history.

The bundled Guile module `(cind core)` owns the default binding tables, while `(cind emacs)` owns
the corresponding input strategy. The installer binds only commands present in the application
composition, so optional capabilities can join the shared TUI and GUI keymap without duplicating
policy in C++. The keymap registry and precedence rules remain C++ mechanisms.

The bundled `(cind input)` module provides the reusable `input.read-key` transient state. It owns
one-key capture lifetime, feedback, automatic pop, and cancellation cleanup; a Scheme continuation
maps the captured key to a typed command dispatch. Meow, Vim, and Helix use this mechanism for
register and Thing prompts instead of defining parallel strategy-specific input loops.

The bundled `(cind meow)` strategy demonstrates handler-based translation without a parallel input
loop. Its keypad queries base layers 3–9 for the invoking Window, excluding its own state maps,
resolves translated sequences with the same registry function used by dispatch, and publishes the
same layered completions used by prefix help. Control-to-literal and transparent original-sequence
fallback remain Scheme policy. Multiple Views may independently use Emacs, meow, or another named
strategy.

The bundled `(cind vim)` strategy defines normal, insert, and visual durable states. Delete enters
a transient operator state whose tentative character range is immediately published as the View
Selection. Motion and text-object capture replace that preview through the shared Motion and Thing
registries before dispatching the region verb. Decimal counts and named registers captured through
`input.read-key` remain in the command loop prefix slot across the operator state. `C-c v` selects the strategy from the
default Emacs map.

The bundled `(cind helix)` strategy defines normal, select, and insert durable states. The normal
and select maps bind the same Motion mechanisms to replace and extend transforms respectively, so
each command operates over the full View Selection. `mi` and `ma` enter the shared single-key
capture state, and deletion consumes the resulting ranges through the shared atomic selection verb.
`C-c h` selects the strategy from the default Emacs map.

## Command loop and prefix help

One `CommandLoop` follows the application's active input focus. It consumes the ordered scoped
keymap stack and always-active override maps produced by Scheme policy, and owns a pending multi-key
sequence, its highest-precedence matching keymap, and a pending command prefix containing an
optional count, register, and named typed extras. Dispatch returns a structured status: not
handled, keymap prefix, command-prefix update, executed, awaiting input, disabled, cancelled, or
error.

A prefix command returns a complete replacement prefix value. Keymap layer refreshes retain that
value, and command dispatch chains inherit it. The next ordinary terminal command receives the
prefix in its immutable invocation and consumes the slot. Undefined input, errors, disabled
commands, interaction requests, and `keyboard.quit` also clear it. A single unbound key remains
available to the platform text-input path. When that key is printable and the focused state accepts
text, its prefix remains available until the paired text commit consumes it.

The bundled Emacs map binds `C-u` to a transient `emacs-universal` handler. Its initial count is
four, repeated `C-u` multiplies the count by four, decimal keys replace or extend the numeric
argument, and `-` supplies a negative argument. The first ordinary key pops the handler and is
redispatched through the durable state with the prefix intact. `C-g` removes the transient state
and prefix together. Backspace and Delete in this state dispatch the explicit raw-deletion escape
commands while retaining the same prefix lifecycle. An unbound printable key enters the normalized
text-input path, which repeats the committed UTF-8 text by the positive count without synthesizing
key events; zero inserts nothing and a negative count reports an input error.

The bundled meow normal map binds decimal digits to selection expansion. `SPC` dispatches through
the configured `C-c` leader; `SPC 0` through `SPC 9` begin a transient numeric state in which
following digits accumulate the command count. The first non-numeric key pops that state and is
redispatched through the durable state maps with the count intact. `-` supplies the corresponding
negative argument. `"` pushes a single-key handler state that captures a named register. Count and
register values compose through the shared invocation contract. The formatted prefix is projected
into the echo area independently of pending keymap chords and transient-state feedback.

An expandable Meow Selection publishes the next ten expansion destinations through the normal
state's position-hint provider. Labels `1` through `9` and `0` correspond to expansion amounts one
through ten. Keypad, universal-argument, numeric, register, and single-key capture states obscure
these labels while they own input; returning to normal state derives them from the resulting
Selection.

Thing and Motion registries are application-owned named mechanism tables. Scheme definitions select
pair, CST-node, character-class, fallback, and directional motion mechanisms; evaluation receives
the current immutable document snapshot, CST, and typed Selection. A motion returns a complete
multi-range Selection for move or extend behavior. A thing returns inner and bounds ranges, while
the effective major/minor mode policy maps semantic nouns to concrete definitions. Meow's `w/b`
and `,`/`.` bindings consume these APIs without adding modal branches to the command loop.

Selection verbs submit a typed Selection and one replacement per range to the native edit session.
Character, line, and node ranges are resolved before mutation, checked for overlap, and committed
as one transaction and undo node. The mechanism returns the collapsed post-edit Selection, so the
strategy package remains responsible for the visible selection lifecycle.

Text input targets every Selection head through the same transaction boundary. Typed ASCII input
runs on-input indentation for all affected lines in that transaction; IME text, paste, and yank
insert one shared string or positional strings at the heads. Coincident heads are deduplicated.
Scheme kill entries retain one string per range, use a newline-separated projection for the platform
clipboard, and restore positional entries when the yank target has the same range count. Region
copy and kill resolve granularity through the native Selection mechanism rather than selecting only
the primary range.

The bundled `(cind structural)` policy pushes `structural-node` above the invoking View's durable
state. Scheme owns the history lifecycle, while the View registry stores each complete Selection as
anchors and settles it across transactions. The native noun mechanism computes one all-or-none CST
expansion across every range and assigns `node` granularity. Expansion and contraction remain in the
state; deletion pops it before dispatching the common selection verb. Exiting preserves the selected
nodes and returns to the underlying input strategy.

The keymap registry merges immediate continuations across ordered keymaps and their parents using
the same precedence and remap pass as dispatch. While a keymap sequence or input-state feedback is
pending, the scene contains a popup listing the next keys and their semantic details. This is the
which-key view. Runtime, mode, inherited, translated, and user bindings appear without a separate
frontend help table.

Commands receive `CommandContext`, which names the runtime, window, buffer, and view explicitly.
Settings resolution follows the same context. Command callbacks do not depend on process-global
editor state.

## Interaction protocol

A command returns `CommandCompleted`, `InteractionRequest`, or a named `CommandDispatch`. A request
describes a text prompt or candidate picker using prompt text, initial input, a history name, a
candidate-provider name, an accept command ID, and typed arguments. The command loop returns the request to
`EditorApplication`; `InteractionController` creates the transient minibuffer Buffer/View/Window,
records the originating command target, and owns provider state while the frontend continues its
normal event loop.

The minibuffer Buffer contains the single-line UTF-8 input and undo history, while its View owns the
anchor-backed caret, selection, settings, InputState stack, and local keymap. Text commits and
ordinary editor commands mutate that View through the same EditSession path as document editing;
revision changes refresh candidates. Enter and Escape remain interaction-local. The picker child
map adds `C-n`/Down/Tab and `C-p`/Up for candidate navigation. Submission destroys the transient
Window, View, and Buffer and returns a named dispatch with the saved origin target. The command loop
switches to that explicit target before invoking the accept command, so the accept callback sees the
document context even though submission began in the minibuffer. An accept command may return
another request, which supports multi-step interactions without retaining a C++ closure.

Each request may name a history shared by later requests in the same application. Successful,
non-empty submissions append to that bounded history. `M-p` moves from the current minibuffer draft
toward older entries, and `M-n` moves toward newer entries before restoring the saved draft. Editing
a recalled value exits history traversal while leaving the edited text in the ordinary minibuffer
Buffer. History replacement updates the same View caret and candidate generation as direct editing.

Candidate providers return semantic values, labels, details, and filter text immediately, through
a cancellable native worker job, or through a scripted async request paired with an editor-thread
result transform. Provider preparation runs on the editor thread and captures immutable worker
input; filesystem traversal and large candidate ranking run through the async runtime. Every input
change receives a monotonically increasing generation, and only results for the active generation
can update the interaction. An asynchronous refresh retains the last complete candidate snapshot
until the replacement is ready, then swaps the candidate list atomically.
Command, key-binding, open-buffer, and filesystem
providers implement the command palette, key help, buffer switching, and file opening. GUI and TUI
render the same candidate and loading state through frontend-specific layout. Interactive pickers
own a stable minibuffer band containing the prompt, input and candidate rows; the echo area is a
separate message surface. Prefix help uses the same structured popup content without an input.

## Extension boundary

Command, keymap, mode, InputState, and provider names, generational editor IDs, `SettingValue`
arguments, `CommandContext`, and interaction requests form the Guile host boundary.
`CommandContext` explicitly identifies the Window, View, Buffer, Project, and Runtime involved in an
invocation. Scheme receives explicit host capabilities instead of frontend objects or a
process-global current editor. Definitions are sealed after configuration; window, buffer, view,
project, and application values remain mutable through
explicit runtime APIs. [Guile scripting architecture](scripting.md) defines interpreter ownership,
module loading, error containment, and the development-service boundary.

## Inspection

The GUI inspector exposes `editor.command_loop`, `editor.input_state`, `editor.selection`, `editor.scripting`,
`editor.interaction`, `editor.buffers`, `editor.windows`, `editor.location_navigation`, and
`editor.focus`. Command-loop state includes
keymap names with their scopes and parent chains, override maps, pending keys, the highest matching
keymap, the input state owning handler feedback, pending count/register/extras, formatted prefix,
and last command. Interaction state includes the minibuffer Window/View/Buffer identities, origin
target, prompt kind, input caret, provider, selection, generation, errors, and candidates. Buffer
state includes resource and lifecycle data; Window state identifies each Window's bound View and
Buffer. Input-state inspection
reports the active state name, text-input policy, selection-after-edit policy, cursor shape, and
indicator, plus whether it owns a handler, lifecycle callbacks, or a position-hints provider.
Selection state reports whether a mark is active, the primary range, strategy metadata, and every
directional range with its granularity.
The popup is also represented as `scene.region.popup`, including structured title, input, visible
item window, global item count and selection alongside its cell geometry, surface, and overlay
anchor. Each frontend projects this semantic content into its native layout.
