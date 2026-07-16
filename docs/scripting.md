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
defines the public command value API, `(cind emacs)` and `(cind toy-modal)` define input strategies,
and `(cind core)` composes the built-in editor policy. These modules are loaded before application
keymaps are configured. Calls from C++ enter Guile through a condition boundary; a Scheme condition
becomes a C++ error value and is retained in the scripting inspection snapshot. C++ exceptions
raised by a host primitive are translated into Scheme conditions.

`editor.scripting` exposes the engine and Guile version, loaded policy modules, scripted command,
provider, input-state, and mode counts, command/provider/keymap/input-state/mode installation
revisions, and the most recent error. This state is diagnostic and is not a plugin ABI.

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
(define-input-state! host name keymaps text-input cursor indicator handler-or-#f)
(set-base-input-state! host view-id state-name)
(push-input-state! host view-id state-name)
(pop-input-state! host view-id)
(reset-input-states! host view-id)
(view-input-states host view-id)
(observe-input-state-changes! host procedure)
(%define-mode! host name kind parent keymap interaction-class initial-state things)
(mode-properties host mode-name)
(%set-interaction-class-state! host interaction-class state-name-or-#f)
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
(set-selection! host view-id anchor head)
(clear-selection! host view-id)
(buffer-substring host buffer-id start end)
(erase-range! host view-id start end)
(insert-text! host view-id text)
(soft-kill-range host view-id)
(set-view-caret! host view-id offset)
(reset-preferred-column! host view-id)
(structural-motion-target host view-id motion)
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

`define-input-state!` creates or reconfigures a named state. `keymaps` is an ordered proper list or
vector of keymap names; `text-input` is `accept` or `ignore`; `cursor` is `beam`, `block`, or
`underline`; and `indicator` is presentation text. An optional handler receives the View ID and
canonical key notation. It returns `pass`, `consume`, or `#(dispatch command-name)`. Handler errors
are retained by the scripting runtime and consume the key without escaping into a frontend event
loop.

The state mutation procedures address a generational View ID. Base replacement initializes or
changes the durable state. Push adds a transient state, pop removes one transient state, and reset
removes all transients while preserving the base. `view-input-states` returns the stack from durable
state to top as a vector of names. Every application View is initialized with the `emacs` state
defined by `(cind emacs)`; its empty state keymap list preserves the default Emacs keymap policy.
The focused document state's cursor shape and indicator flow through the frontend-independent Scene.
Interactions temporarily present a beam cursor because their text input owns focus.

`observe-input-state-changes!` registers an editor-thread procedure for every base, push, and pop
transition. It receives `#(kind view-id from-state-or-#f to-state-or-#f)`. Observer conditions are
retained as scripting diagnostics and do not roll back the completed state transition or interrupt
other observers.

`(cind core)` wraps `%define-mode!` as keyword procedures `define-major-mode!` and
`define-minor-mode!`. A definition accepts `#:parent`, `#:keymap`, `#:interaction-class`,
`#:initial-state`, and `#:things`; thing bindings are an association list of semantic names to
mechanism kinds. Parent modes have the same major/minor kind. When a child keymap has no explicit
parent, mode inheritance assigns the nearest parent mode keymap. `mode-properties` returns the
declared metadata together with the effective keymap names.

`set-interaction-class-states!` configures a strategy's `editing` and `interface` durable states.
The native `%set-interaction-class-state!` primitive applies each mapping and immediately rederives
all existing Views. `set-buffer-major-mode!` and `set-buffer-minor-mode!` mutate buffer-scoped mode
state. `buffer-mode-policy` returns `#(interaction-class initial-state things)`. Effective policy
changes update every View of the Buffer and notify procedures registered through
`observe-mode-policy-changes!` with
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

View and text capabilities expose byte offsets as unsigned integers and ranges as two-element
start/end vectors. `view-caret`, `view-mark` and `view-selection` are immutable queries;
`set-selection!` preserves anchor/head direction while `clear-selection!` releases the mark.
`buffer-substring` copies only the requested range. `erase-range!` and `insert-text!` enter the
native edit-session transaction path, which keeps undo, incremental syntax analysis, anchors and
caret reveal coherent. `soft-kill-range` is a syntax-aware range query and does not mutate text.
`structural-motion-target` accepts `forward-expression`, `backward-expression` or `up-list` and
returns the corresponding syntax-derived byte offset or `#f`. `set-view-caret!` changes the
document anchor, while `reset-preferred-column!` clears vertical-motion affinity. Scheme composes
these mechanisms with `request-redraw!` into structural movement commands.

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
vector containing a typed argument vector and an optional repeat count. Command arguments use the
same boolean, integer, real and string domain as `SettingValue`.

A Scheme command imports `(cind command)` and returns one tagged vector through its constructors:

| Tag | Meaning |
| --- | --- |
| `completed` | complete with an optional typed value |
| `error` | return a command error string |
| `dispatch` | continue through the named command pipeline |
| `interaction` | request a text or picker interaction using a named provider and accept command |

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
with the platform clipboard; yank imports the clipboard when that ring is empty. File and project
commands compose native storage, indexing and search capabilities without owning asynchronous
state.

The `(cind core)` module owns the default Emacs-style editor and application keymaps. It defines the
named maps and populates them after built-in commands exist. The `C-x` family is a named prefix map,
so its identity and label are available to completion and inspection UI. Interaction-local editing
and the always-active system escape map are bootstrap mechanisms with their own lifecycle and
precedence contracts.

`(cind toy-modal)` is a small strategy that exercises the same public mechanisms as an extension.
`C-c n` selects its `toy-normal` base state, whose `toy-modal.normal` map provides `h`, `j`, `k`,
`l`, `0`, `$`, and `x`, ignores direct text input, presents a block cursor, and displays `N` in the
modeline. `i` restores the `emacs` base state. Strategy commands address the invoking View rather
than changing application-global input policy.

The same module declares `fundamental-mode`, `prog-mode`, and `special-mode`. Fundamental and
programming buffers have the `editing` interaction class; special buffers have `interface`.
The built-in C++ mode derives from `prog-mode`, and generated location-list buffers derive from
`special-mode`. The default Emacs strategy maps both classes to the `emacs` InputState.

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
