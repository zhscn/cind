# Guile scripting architecture

cind uses Guile as its policy and extension language. C++ owns editor mechanisms: generational
Buffer, View, Window, Project and Mode objects; registries; text mutation; asynchronous I/O; command
dispatch; and frontend-independent UI state. Scheme modules compose those mechanisms into editor
behavior.

```text
bundled Scheme modules
          │ named procedures and immutable values
          v
     GuileRuntime ───── explicit host capability ────> EditorRuntime registries
          │                                                │
          └──── caught Scheme condition                    ├─ command/keymap
                    │                                      ├─ buffer/view/window
                    v                                      └─ interaction provider
             inspector state
```

## Runtime ownership

Each `EditorApplication` owns one `GuileRuntime`. The runtime is created and used on the editor
thread and outlives every Scheme callback registered for that application. Guile VM initialization
is process-wide, while access to an editor instance is represented by an explicit foreign host
object. Scheme code does not resolve an implicit current application.

The bundled Scheme tree is copied into the build directory as a runtime resource. `(cind command)`
defines the public command value API; `(cind emacs)`, `(cind helix)`, `(cind meow)`, `(cind vim)`,
and `(cind toy-modal)` define input strategies; `(cind structural)` defines structural selection
policy; and `(cind core)` composes the built-in editor policy.
These modules are loaded before application keymaps are configured. Calls from C++ enter Guile
through a condition boundary; a Scheme condition becomes a C++ error value and is retained in the
scripting inspection snapshot. C++ exceptions raised by a host primitive are translated into
Scheme conditions.

`editor.scripting` exposes the engine and Guile version, loaded policy modules, scripted command,
provider, input-state, input-strategy, and mode counts, command/provider/keymap/input-state/mode
installation revisions, and the most recent error. This state is diagnostic and is not a plugin
ABI.

## Host capabilities

`(cind host)` is the native module presented to Scheme. Its procedures require the foreign host
object as an explicit first argument. A host object is valid only for its owning application and
does not expose raw editor pointers to Scheme.

The native module exports:

```scheme
(define-command! host command-name execute enabled)
(define-interaction-provider! host provider-name complete)
(define-keymap! host keymap-name parent-or-#f)
(bind-key! host keymap-name key-sequence command-or-prefix)
(bind-key-if-command! host keymap-name key-sequence command-name)
(bind-remap! host keymap-name command-name replacement-name)
(keymap-bindings host keymap-name)
(resolve-key-sequence host keymap-names key-sequence)
(base-keymap-layers host context)
(key-sequence-completions host keymap-names key-sequence)
(set-input-feedback! host view-id sequence hints)
(clear-input-feedback! host view-id)
(define-input-state! host name keymaps text-input cursor indicator handler-or-#f)
(define-input-strategy! host name editing-state interface-state selection-after-edit)
(set-default-input-strategy! host strategy-name)
(set-view-input-strategy! host view-id strategy-name-or-#f)
(view-input-strategy host view-id)
(set-base-input-state! host view-id state-name)
(push-input-state! host view-id state-name)
(pop-input-state! host view-id)
(reset-input-states! host view-id)
(view-input-states host view-id)
(observe-input-state-changes! host procedure)
(define-thing! host name pattern)
(define-motion! host name mechanism)
(%define-mode! host name kind parent keymap interaction-class initial-state things)
(mode-properties host mode-name)
(set-buffer-major-mode! host buffer-id mode-name-or-#f)
(set-buffer-minor-mode! host buffer-id mode-name enabled?)
(buffer-mode-policy host buffer-id)
(observe-mode-policy-changes! host procedure)
(enabled-command-names host context)
(open-buffer-summaries host)
(project-root host project-id)
(project-files host project-id)
(active-key-bindings host)
(buffer-id-by-name host name)
(buffer-resource host buffer-id)
(path-parent host path)
(path-relative host path base)
(path-filename host path)
(directory-path? host path)
(path-as-directory host path)
(view-caret host view-id)
(view-mark host view-id)
(view-selection host view-id)
(set-selection! host view-id selection)
(clear-selection! host view-id)
(push-selection-history! host view-id selection)
(pop-selection-history! host view-id)
(clear-selection-history! host view-id)
(selection-history-depth host view-id)
(replace-selection! host view-id selection replacement-or-vector)
(selection-texts host view-id selection)
(buffer-substring host buffer-id start end)
(erase-range! host view-id start end)
(insert-text! host view-id text-or-vector)
(soft-kill-range host view-id)
(set-view-caret! host view-id offset)
(reset-preferred-column! host view-id)
(thing-selection host view-id selection thing-name inner-or-bounds)
(motion-selection host view-id selection motion-name count extend?)
(expand-node-selection host view-id selection)
(write-clipboard! host text)
(read-clipboard host)
(display-buffer! host window-id buffer-id)
(move-caret-to-line! host view-id zero-based-line zero-based-display-column)
(set-message! host message)
(ensure-project-index! host project-id)
(open-file! host window-id path)
(start-project-search! host project-id window-id query)
(set-buffer-resource! host buffer-id path)
(save-buffer! host buffer-id)
(open-buffer-ids host)
(kill-buffer! host buffer-id force?)
(request-quit! host force?)
(split-window! host window-id axis)
(delete-window! host window-id)
(delete-other-windows! host window-id)
(select-other-window! host window-id delta)
(request-redraw! host)
```

`define-command!` registers a Scheme procedure in `CommandRegistry`. `execute` receives an
immutable command context and invocation value. `enabled` receives the same context and is either a
predicate or `#f`. The runtime protects registered procedures from collection and invalidates them
with their owning application.

`define-keymap!` creates or reconfigures a named keymap with an explicit parent. Names may be
symbols or strings. Repeating the definition is idempotent, which lets policy installation refresh
bindings after optional commands become available.

`bind-key!` accepts a command name or `(prefix keymap-name [label])`. A later write replaces the
command or prefix stored at that sequence. `bind-remap!` substitutes a resolved command in that
keymap without changing its key sequences. Parent and named-prefix cycles raise a Scheme
condition. `bind-key-if-command!` is the optional-capability variant: an absent command returns
`#f`; all other invalid references and key sequences raise a condition.

`keymap-bindings` returns effective command, prefix and remap entries as tagged vectors. Command
entries have the form `#(command keys command #f)`, prefix entries are
`#(prefix keys keymap-or-#f label-or-#f)`, and remaps are
`#(remap command replacement #f)`. `resolve-key-sequence` accepts an ordered list or vector of
keymap names and returns `#(none)`, `#(prefix source-keymap)`, or
`#(command command-name source-keymap)`. Resolution is side-effect free and applies at most one
remap using the same high-to-low layer order as command dispatch.

`base-keymap-layers` returns the named Window, View, Buffer, minor-mode, major-mode, editor, and
application maps for the Window in a command context. Input-state maps and the system override map
are excluded. `key-sequence-completions` performs the corresponding side-effect-free layered query;
an empty sequence requests root entries. It returns `#(key detail prefix?)` vectors, merges duplicate
keys by layer precedence, and applies the same one-pass remap as resolution. These operations let a
transient translator inspect the command surface beneath itself without depending on an implicit
focused application.

`define-input-state!` creates or reconfigures a named state. `keymaps` is an ordered proper list or
vector of keymap names; `text-input` is `accept` or `ignore`; `cursor` is `beam`, `block`, or
`underline`; and `indicator` is presentation text. An optional handler receives a complete command
context and canonical key notation. It returns `pass`, `consume`, `#(dispatch command-name)`,
`#(dispatch command-name arguments)`, or `#(pending sequence hints)`. Dispatch arguments are typed
command values, and a dispatch without an explicit prefix inherits the command loop's pending
count, register, and extras. Pending consumes the key and publishes the supplied
`#(key detail prefix?)` hint vector through the same frontend-independent popup channel as keymap
prefixes. Handler errors are retained by the scripting runtime and consume the key without escaping
into a frontend event loop. The system override map is resolved before a handler, so `C-g` remains
an unconditional escape path.

An input strategy is a named mapping from the `editing` and `interface` interaction classes to
durable states. Its `selection-after-edit` policy is `collapse` or `preserve` and applies when a
document-changing command delegates its resulting selection to the strategy. Direct text and IME
input consult the same policy after committing an edit. The application has a default strategy,
while each View may select an independent override. Passing `#f` to
`set-view-input-strategy!` restores inheritance from the application default. Selecting a strategy
immediately rederives the View's durable state from its Buffer mode; a mode-specific `initial-state`
remains the higher-priority override.

The state mutation procedures address a generational View ID. Base replacement initializes or
changes the durable state. Push adds a transient state, pop removes one transient state, and reset
removes all transients while preserving the base. `view-input-states` returns the stack from durable
state to top as a vector of names. Every application View is initialized with the `emacs` state
defined by `(cind emacs)`; its empty state keymap list preserves the default Emacs keymap policy.
The focused document state's cursor shape and indicator flow through the frontend-independent Scene.
Interactions temporarily present a beam cursor because their text input owns focus.

`(cind input)` defines the shared `input.read-key` transient state and exposes
`(read-key-then! host view-id procedure #:sequence text #:hints vector)`. A strategy supplies a
per-View continuation, display sequence, and hint vector;
the helper pushes the state, captures exactly one canonical key, pops before invoking the
continuation, and removes the continuation when an override command cancels the state. Register,
Thing, and other one-key strategy prompts therefore share one lifetime and dispatch path while
their interpretation remains Scheme policy.

Input feedback belongs to the View's current state stack. `set-input-feedback!` publishes feedback
when a command enters a transient state, while a handler may replace it with a pending result.
`clear-input-feedback!` removes it explicitly. A push, pop, base transition, or next delivered key
invalidates the prior feedback, so hints cannot outlive the state that produced them. Switching
Windows preserves each View's independent transient state and feedback.

`observe-input-state-changes!` registers an editor-thread procedure for every base, push, and pop
transition. It receives `#(kind view-id from-state-or-#f to-state-or-#f)`. Observer conditions are
retained as scripting diagnostics and do not roll back the completed state transition or interrupt
other observers.

`(cind core)` wraps `%define-mode!` as keyword procedures `define-major-mode!` and
`define-minor-mode!`. A definition accepts `#:parent`, `#:keymap`, `#:interaction-class`,
`#:initial-state`, and `#:things`; thing bindings are an association list of semantic names to
named Thing definitions. Parent modes have the same major/minor kind. When a child keymap has no explicit
parent, mode inheritance assigns the nearest parent mode keymap. `mode-properties` returns the
declared metadata together with the effective keymap names.

`set-buffer-major-mode!` and `set-buffer-minor-mode!` mutate buffer-scoped mode state.
`buffer-mode-policy` returns `#(interaction-class initial-state things)`. Effective policy changes
rederive every View of the Buffer through that View's selected strategy and notify procedures
registered through `observe-mode-policy-changes!` with
`#(kind buffer-id mode-name-or-#f before-policy after-policy)`. A mode's explicit initial state
precedes the class mapping, and the most recently enabled minor-mode declaration precedes the major
mode.

`define-interaction-provider!` registers an editor-thread Scheme completion procedure. The
procedure receives an immutable command context and query string and returns a vector of
four-element candidate vectors containing value, label, detail and filter text. The runtime
protects the procedure from collection, validates the complete result and invalidates the callback
with its application. Scheme conditions and malformed candidates become interaction errors and are
retained in the scripting inspection snapshot.

`enabled-command-names`, `open-buffer-summaries`, `project-root`, `project-files` and
`active-key-bindings` expose immutable registry and application snapshots. Path operations provide
platform-native relative, filename and parent calculations. These capabilities keep candidate
selection and presentation in Scheme while C++ retains registry identity and filesystem syntax.

`buffer-id-by-name` resolves a buffer name to its generational ID or `#f`. `display-buffer!` assigns
that buffer to the target window through the application view lifecycle. `move-caret-to-line!`
moves a view caret using zero-based logical line and display-column coordinates, clamps the line to
the document, resets vertical-motion state and requests caret reveal. `set-message!` replaces the
application message. Mutating capabilities execute synchronously on the owning editor thread and
raise a Scheme condition when the ID or requested transition is invalid.

View and text capabilities expose byte offsets as unsigned integers. Ordinary text ranges use
two-element start/end vectors. `view-selection` returns
`#(selection primary metadata ranges)`, where `ranges` is a non-empty vector of
`#(anchor head granularity)` values. The granularity is `char`, `line`, `block`, or `node`;
`primary` is the zero-based range index, and `metadata` is an arbitrary Scheme datum owned by the
input strategy. A View without an active region returns one collapsed `char` range at the caret,
while `view-mark` returns `#f`. The `(cind command)` module provides `selection-range` and
`selection` constructors, field accessors, and `selection-with-ranges` / `selection-with-metadata`
copy helpers for this representation. `set-selection!` preserves range direction, primary identity,
granularity, and metadata. `clear-selection!` collapses the model to its primary head and releases
the active mark.

Selection endpoints are document anchors. Direct document transactions settle every endpoint
across insertions and erasures, and moving the View caret moves the primary head without changing
the other ranges. Views that display the same Buffer retain independent selections. Selection
history is also View-owned and anchor-backed. Scheme explicitly pushes, pops, and clears complete
Selection values; `selection-history-depth` exposes its current stack size. This mechanism assigns
no meaning to history entries or state transitions.
`selection-texts` extracts one string per range using character, line, and node granularity.
`buffer-substring` copies an explicit byte range. `erase-range!` and `insert-text!` enter the native
edit-session transaction path, which keeps undo, incremental syntax analysis, anchors and caret
reveal coherent. A string passed to `insert-text!` is inserted at every Selection head; a vector
provides one positional string per range. Coincident heads share one insertion and require the same
text. All heads are edited in one transaction. `replace-selection!` similarly accepts either one
replacement string for every range or a vector containing one string per range. It resolves
character, line, and node granularities, rejects overlapping or block ranges, applies every
replacement in one transaction, and returns the collapsed post-edit Selection with its primary
index and metadata intact. `soft-kill-range` is a syntax-aware range query and does not mutate text.

`define-thing!` creates or reconfigures a runtime-owned named noun definition. Patterns are
`(pair open close)`, `(cst-node kind)`, `(char-class word-or-symbol)`, or
`(multi pattern ...)`. Pair evaluation prefers a matching CST group before textual fallback;
`cst-node` resolves the innermost matching syntax node or literal token. `thing-selection` resolves
a semantic mode binding before a concrete registry name and evaluates the requested inner or bounds
extent at every head in the supplied Selection. It preserves range order and the primary index,
returning `#f` unless every range resolves. Returned metadata records the semantic name, concrete
definition, and extent.

`define-motion!` maps a name to a native pure mechanism: forward/backward character, word, symbol,
or expression, plus up-list. `motion-selection` transforms every range in the supplied Selection,
applies a signed count, and either collapses each range at its destination or preserves its anchor
for extension. The View identifies the snapshot and effective mode; the Selection is explicit input
data rather than hidden View state. Both APIs return the same full Selection value used by command
results, so Scheme commands can compose noun operations without intermediate UI mutation.
`reset-preferred-column!` clears vertical-motion affinity and `request-redraw!` requests caret
reveal.

`expand-node-selection` is a pure CST query over its complete Selection input. It replaces every
range with its next enclosing syntactic range, preserves direction, primary identity and metadata,
and marks each result with `node` granularity. It returns `#f` if any range has no enclosing node, so
policy never observes a partially expanded multi-range Selection.

`write-clipboard!` returns `#f` on success or an error string. `read-clipboard` returns a
two-element vector containing an optional string and optional error. The absence of a platform
clipboard produces two `#f` values. The Scheme policy retains editor-local data independently of
clipboard success. This result domain lets Scheme own kill-ring policy without making terminal OSC
52 or GUI clipboard objects part of the scripting ABI.

`buffer-resource` returns a buffer's resource path or `#f`. `path-parent`, `directory-path?` and
`path-as-directory` expose platform filesystem syntax without embedding separator rules in Scheme.
`set-buffer-resource!` normalizes a path, changes the buffer to file-backed storage, derives its
display name and attaches the matching project. File commands use these primitives to keep prompt
and save-as policy in Scheme. `save-buffer!` snapshots the identified buffer and schedules the
native atomic-write pipeline without consulting frontend focus.

`open-buffer-ids` returns the application-owned buffers in lifecycle order. `kill-buffer!` applies
the native buffer/view/window teardown contract and returns `#f` on success or an expected error
string. Scheme uses the snapshot to implement wrap-around next/previous policy and turns kill
refusals into ordinary command errors.

Window capabilities operate on an explicit command-context window. `split-window!` accepts `rows`
or `columns`; split, delete and focus operations return `#f` on success or an expected error string.
`delete-other-windows!` retains the identified window, `request-quit!` applies the application's
unsaved-buffer quit contract, and `request-redraw!` requests caret reveal. Scheme maps these
mechanisms to the default window and application commands.

`ensure-project-index!` idempotently schedules the native asynchronous indexer for a project.
`open-file!` normalizes and opens a resource through the asynchronous file pipeline, targeting the
given window. `start-project-search!` runs the native project search pipeline and directs its result
buffer to the given window. These procedures initiate work and return after the operation has been
accepted; completion, cancellation and failure are applied later on the editor thread.

## Scripted commands

A command context is an association list containing generational `window`, `buffer`, `view`, and
optional `project` IDs. Each ID is a two-element vector of slot and generation. An invocation is a
`#(invocation arguments count register extra)` vector. `arguments` is the positional typed argument
vector; `count` and `register` are values or `#f`; and `extra` is an association list from names to
typed values. Command arguments and prefix extras use the same boolean, integer, real and string
domain as `SettingValue`. `invocation-arguments`, `invocation-repeat-count`,
`invocation-register`, and `invocation-prefix-extra` provide named accessors.

A Scheme command imports `(cind command)` and returns one tagged vector through its constructors:

| Tag | Meaning |
| --- | --- |
| `completed` | complete with an optional typed value |
| `completed-preserve` | complete and preserve the current selection |
| `completed-collapse` | complete and collapse the selection to its primary head |
| `completed-selection` | complete with a full replacement Selection and optional typed value |
| `prefix` | replace the command loop's pending count/register/extra slot |
| `error` | return a command error string |
| `dispatch` | continue through the named command pipeline |
| `interaction` | request a text or picker interaction using a named provider and accept command |

`command-completed`, `command-completed/preserve`, `command-completed/collapse`, and
`command-completed/selection` construct the four completion forms. The default completion applies
the active InputStrategy policy only when the command chain changed its context Buffer. Explicit
preserve, collapse, and replacement results apply independently of document revision. A dispatch
chain is one command lifecycle: only its terminal completion selects the explicit result, while the
initial and final Buffer revisions determine whether the strategy default applies.

`command-prefix` constructs a prefix update from a count or `#f`, a register name or `#f`, and a
proper extra association list. A prefix update remains pending across keymap-layer changes. The next
ordinary command receives it; dispatch results inherit it through the terminal command. Terminal
completion, interaction, failure, disabled lookup, undefined input, and keyboard quit consume it.

The bridge validates the complete value before constructing a C++ command action. Named dispatch
and accept commands are resolved through the invoking `EditorRuntime`; a script cannot inject a
foreign registry ID. Scheme conditions and bridge exceptions become `CommandError` values and are
retained in `editor.scripting.last_error`.

`(cind core)` defines the command palette and its dispatching accept command, file-open and save-as
interactions, named and relative buffer switching, buffer kill policy, goto-line parsing and
movement, window layout and application quit policy, project file selection and project search with
project-aware enabled predicates, key-help selection and message presentation, mark/region
commands, structural expression/list movement, and kill/yank behavior. Each installed core policy
owns a bounded kill ring in its Scheme closure. Copy and kill commands synchronize the newest entry
with the platform clipboard and also write the invocation's named register when present. A yank
with a named register reads that register; an unnamed yank reads the newest kill and imports the
clipboard when the ring is empty. File and project commands compose native storage, indexing and
search capabilities without owning asynchronous state.

The `(cind core)` module owns the default Emacs-style editor and application keymaps. It defines the
named maps and populates them after built-in commands exist. The `C-x` family is a named prefix map,
so its identity and label are available to completion and inspection UI. Interaction-local editing
and the always-active system escape map are bootstrap mechanisms with their own lifecycle and
precedence contracts.

`(cind toy-modal)` is a small strategy that exercises the same public mechanisms as an extension.
`C-c n` selects the `toy-modal` strategy for the invoking View. Its `toy-normal` editing state and
`toy-modal.normal` map provide `h`, `j`, `k`, `l`, `0`, `$`, and `x`, ignore direct text input,
present a block cursor, and display `N` in the modeline. `i` selects the `emacs` strategy for that
View. The toy strategy maps interface Buffers to the Emacs state, demonstrating class-specific
behavior without package-specific routing.

`(cind core)` declares `fundamental-mode`, `prog-mode`, and `special-mode`. Fundamental and
programming buffers have the `editing` interaction class; special buffers have `interface`.
The built-in C++ mode derives from `prog-mode`, and generated location-list buffers derive from
`special-mode`. The default Emacs strategy maps both classes to the `emacs` InputState.

`(cind meow)` is a modal strategy implemented entirely through public host mechanisms. `C-c m`
selects it for the invoking View. Editing Buffers derive `meow-normal`, interface Buffers derive the
sparse `meow-motion`, and `i` enters the text-accepting `meow-insert` state. Normal and motion maps
bind `SPC`, `x`, `c`, `g`, and `m` to a transient `meow-keypad` handler. The handler translates
unmodified keys against base layers, publishes completions, applies control-to-literal fallback,
and finally tries the original key sequence for transparent interface bindings. Thus `x c`
dispatches `C-x C-c`, `x b` falls back to `C-x b`, `m x` dispatches `M-x`, and interface keymaps
remain available without package-specific routing. `SPC 0` through `SPC 9` resolve through the
`C-c` leader to prefix commands and enter `meow-numeric`; subsequent digits are accumulated by that
transient handler until an ordinary key falls through to the durable normal map. Normal-state
digits instead expand the active directed Selection by the numbered position, with `0` selecting
the tenth position. Word and symbol commands store their selection type and paired expansion
motions in the Selection metadata, while the native motion registry remains a pure evaluator of an
explicit Selection value. The normal map binds `"` to the shared single-key capture state. Its
captured key replaces the invocation's register while preserving an already accumulated count and
extra prefix values; cancellation removes the transient session and the pending prefix. `,` and `.`
use the same capture state for inner and bounds selection. `char-thing-table-add!` owns the
strategy's configurable character-to-semantic-thing table; the bundled table maps `a`, `w`, and `s`
to the active mode's angle, word, and string definitions.

`(cind helix)` is a selection-first modal strategy selected with `C-c h`. Its durable `hx-normal`,
`hx-select`, and `hx-insert` states use separate declarative keymaps. Normal motions replace every
range in the View Selection; select motions apply the same registered Motion with extension enabled,
preserving each anchor. The `mi` and `ma` prefixes use `read-key-then!` to resolve the active mode's
inner or bounds Thing at every range head. Delete dispatches the
shared atomic selection verb, and the strategy preserves multi-range selection results across edits.

`(cind structural)` owns the sticky `structural-node` transient state selected with `C-c e`. Scheme
controls a per-View anchored selection-history stack. `e`, `l`, and `]` expand every range;
`s`, `h`, and `[` restore the previous Selection; `d` leaves the state and dispatches the shared
atomic selection verb; and `ESC` or `q` leaves the state while preserving the selected nodes. Native
code provides the generic anchored history and CST query mechanisms; Scheme assigns their
structural semantics and bindings.

## Scripted interaction providers

`(cind core)` defines the synchronous `commands`, `buffers`, `project-files` and `key-bindings`
providers. Scheme filters internal commands and formats semantic candidate fields from native
snapshots. Query ranking remains in `InteractionController`, so native and scripted providers share
the same ordering, selection and viewport behavior.

The `files` provider remains native because directory enumeration is cancellable worker-thread I/O.
It captures immutable query and resource values, runs through `AsyncRuntime`, and returns the same
candidate structure to `InteractionController`. Scripted providers are editor-thread procedures and
do not perform blocking work.

Additional host APIs follow the same boundary:

- pass stable names, generational IDs, immutable snapshots and typed values across languages;
- mutate editor state only on the editor thread;
- return command actions and interaction requests as data;
- schedule blocking work through `AsyncRuntime` and apply completions on the editor thread;
- keep frontend, Skia, SDL and terminal objects outside the scripting ABI.

## Ares development-service design

[Guile Ares RS](https://git.sr.ht/~abcdw/guile-ares-rs) fits the development backend for Scheme
source. Ares is a Guile library and nREPL-compatible RPC server; the `rs` suffix means RPC Server.
Its protocol provides interruptible asynchronous evaluation, streamed output, stdin, completion,
symbol lookup, documentation, arglists, sessions and backtraces.

The optional provider uses Ares as a separately installed service and implements the nREPL bencode
client over `AsyncRuntime`. This keeps Ares's GPL-3.0-or-later sources outside the cind source tree
and avoids coupling the editor build to Ares internals. Protocol results map onto existing editor
mechanisms:

| Ares/nREPL result | cind mechanism |
| --- | --- |
| completion candidates | interaction provider |
| definition file, line and column | source location and location navigation |
| evaluation values and streamed output | generated result buffer |
| backtrace frames | location-list buffer |
| interrupt and session lifecycle | cancellable asynchronous operation |

A standalone Ares process supplies project-wide Guile development without sharing editor memory.
An in-process endpoint uses the same Guile VM as `GuileRuntime`, which makes `(cind host)` and loaded
cind policy modules available for live inspection and evaluation. The in-process endpoint receives
an explicit host capability and uses Ares fibers for evaluation; it does not call editor mechanisms
from an evaluation fiber. Host requests are marshalled to the editor thread.

Current Ares releases require Guile 3.0.10 or newer, `fibers`, and the custom-port facilities
included with that Guile release. The base scripting host uses the `guile-3.0` pkg-config API; the
in-process Ares endpoint is enabled only when those stronger development-service dependencies are
available.
