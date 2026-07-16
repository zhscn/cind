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
defines the public command value API, and `(cind core)` defines the built-in policy. Both modules
are loaded before application keymaps are configured. Calls from C++ enter Guile through a
condition boundary; a Scheme condition becomes a C++ error value and is retained in the scripting
inspection snapshot. C++ exceptions raised by a host primitive are translated into Scheme
conditions.

`editor.scripting` exposes the engine and Guile version, loaded policy modules, scripted-command
count, command and keymap installation revisions, and the most recent error. This state is
diagnostic and is not a plugin ABI.

## Host capabilities

`(cind host)` is the native module presented to Scheme. Its procedures require the foreign host
object as an explicit first argument. A host object is valid only for its owning application and
does not expose raw editor pointers to Scheme.

The native module exports:

```scheme
(define-command! host command-name execute enabled)
(bind-key-if-command! host keymap-name key-sequence command-name)
(buffer-id-by-name host name)
(display-buffer! host window-id buffer-id)
(move-caret-to-line! host view-id zero-based-line zero-based-display-column)
(set-message! host message)
(ensure-project-index! host project-id)
(open-file! host window-id path)
(start-project-search! host project-id window-id query)
```

`define-command!` registers a Scheme procedure in `CommandRegistry`. `execute` receives an
immutable command context and invocation value. `enabled` receives the same context and is either a
predicate or `#f`. The runtime protects registered procedures from collection and invalidates them
with their owning application.

`bind-key-if-command!` resolves named keymaps and commands through `EditorRuntime`. An absent
command returns `#f`, which lets a policy describe bindings for optional application capabilities.
An absent keymap or an invalid key sequence raises a Scheme condition. A successful binding returns
`#t`.

`buffer-id-by-name` resolves a buffer name to its generational ID or `#f`. `display-buffer!` assigns
that buffer to the target window through the application view lifecycle. `move-caret-to-line!`
moves a view caret using zero-based logical line and display-column coordinates, clamps the line to
the document, resets vertical-motion state and requests caret reveal. `set-message!` replaces the
application message. Mutating capabilities execute synchronously on the owning editor thread and
raise a Scheme condition when the ID or requested transition is invalid.

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

`(cind core)` defines the command palette and its dispatching accept command, buffer switching and
its accept command, goto-line parsing and movement, project file selection and project search with
project-aware enabled predicates, and key-help selection and message presentation. Project commands
compose native indexing, file-open and search capabilities without owning asynchronous state.

The `(cind core)` module also owns the default Emacs-style editor and application keymaps. C++
creates the registries and focus hierarchy, then asks the Scheme policy to populate them.
Interaction-local editing and the always-active system escape map are bootstrap mechanisms with
their own lifecycle and precedence contracts.

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
