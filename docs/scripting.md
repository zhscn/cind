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
defines the public command value API; `(cind async)` defines cancellable native task composition;
`(cind minibuffer)` defines high-level text and completion interaction constructors, completion
ranking, candidate navigation, and named history policy;
`(cind lifecycle)` owns configurable startup and fallback-buffer policy and per-application startup
placeholder state;
`(cind pointer)` owns semantic pointer-target behavior;
`(cind extension)` owns isolated source-file loading;
`(cind development)` owns interactive source evaluation and result presentation;
`(cind emacs)`, `(cind helix)`, `(cind meow)`, `(cind vim)`, and `(cind toy-modal)` define input
strategies; `(cind structural)` defines structural selection policy; `(cind introspect)` derives
editor self-description from registry and module state; and `(cind core)` composes the built-in
editor policy.
These modules are loaded before application keymaps are configured. Calls from C++ enter Guile
through a condition boundary; a Scheme condition becomes a C++ error value and is retained in the
scripting inspection snapshot. C++ exceptions raised by a host primitive are translated into
Scheme conditions.

`editor.scripting` exposes the engine and Guile version, loaded policy modules, scripted command,
interaction-provider, input-state, input-strategy, mode, file-mode-rule and project-provider counts,
their installation revisions, loaded extension paths, outstanding script task count, and the most
recent error. This state is diagnostic and is not a plugin ABI.

## User initialization

Interactive frontends discover `cind/init.scm` under `XDG_CONFIG_HOME`, falling back to
`~/.config/cind/init.scm`. Headless rendering and application tests opt in to an explicit init path
so their behavior is independent of the invoking account.

`(cind extension)` evaluates each source file in a fresh Guile module. The module imports
`(cind host)`, `(cind command)`, `(cind async)`, `(cind input)`, `(cind lifecycle)`,
`(cind minibuffer)`, and `(cind pointer)`, and binds the owning application's foreign host as
`host`. File-private definitions remain module-local while registered closures retain access to
them.

An extension load checkpoints the registries and protected Scheme callback ownership before
evaluation. Successful definitions become visible together. A Scheme condition or native bridge
error restores commands, interaction providers, keymaps, input states and strategies, Things,
motions, modes, resource policies, listeners, counters, and protected callback vectors to the
checkpoint. The failed condition is retained as the runtime's latest scripting diagnostic.

## Host capabilities

`(cind host)` is the native module presented to Scheme. Its procedures require the foreign host
object as an explicit first argument. A host object is valid only for its owning application and
does not expose raw editor pointers to Scheme.

The native module exports:

```scheme
(define-command! host command-name execute enabled)
(set-command-documentation! host command-name documentation)
(define-interaction-provider! host provider-name complete)
(define-keymap! host keymap-name parent-or-#f)
(bind-key! host keymap-name key-sequence command-or-prefix)
(bind-key-if-command! host keymap-name key-sequence command-name)
(bind-remap! host keymap-name command-name replacement-name)
(keymap-bindings host keymap-name)
(resolve-key-sequence host keymap-names key-sequence)
(keymap-context-snapshot host context)
(key-sequence-completions host keymap-names key-sequence)
(set-input-feedback! host view-id sequence hints)
(clear-input-feedback! host view-id)
(%define-input-state! host name keymaps text-input cursor indicator handler-or-#f)
(set-input-state-lifecycle! host state-name on-enter-or-#f on-exit-or-#f)
(set-input-state-position-hints! host state-name provider-or-#f)
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
(define-language-profile! host name providers defaults)
(%define-mode! host name kind parent language-profile keymap interaction-class initial-state things)
(define-file-mode-rule! host rule-name mode-name suffixes filenames)
(define-project-provider! host provider-name markers)
(mode-properties host mode-name)
(buffer-language-facet? host buffer-id facet)
(set-buffer-major-mode! host buffer-id mode-name-or-#f)
(set-buffer-minor-mode! host buffer-id mode-name enabled?)
(buffer-mode-policy host buffer-id)
(buffer-mode-summary host buffer-id)
(observe-mode-policy-changes! host procedure)
(enabled-command-names host context)
(command-properties host context command-name)
(open-buffer-summaries host)
(workbench-list host)
(current-workbench host)
(workbench-scope host workbench-id)
(workbench-mru host workbench-id)
(workbench-buffer-summaries host workbench-id widen?)
(workbench-buffer-ids host workbench-id widen?)
(new-workbench! host name project-id-or-#f)
(switch-workbench! host workbench-id)
(close-workbench! host workbench-id)
(adopt-project! host workbench-id project-id)
(expel-buffer! host workbench-id buffer-id)
(workbench-session-state host)
(restore-workbench-session! host serialized-state)
(owned-user-modules host)
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
(buffer-name host buffer-id)
(buffer-text host buffer-id)
(buffer-byte-size host buffer-id)
(buffer-locations host buffer-id)
(set-buffer-locations! host buffer-id locations)
(erase-range! host view-id start end)
(insert-text! host view-id text-or-vector)
(soft-kill-range host view-id 'plain-or-structural)
(set-view-caret! host view-id offset)
(reset-preferred-column! host view-id)
(thing-selection host view-id selection thing-name inner-or-bounds)
(motion-selection host view-id selection motion-name count extend?)
(expand-node-selection host view-id selection)
(write-clipboard! host text)
(read-clipboard host)
(display-buffer! host window-id buffer-id)
(display-generated-buffer! host window-id buffer-name text major-mode style-origin)
(evaluate-scheme! host source source-name)
(move-caret-to-line! host view-id zero-based-line zero-based-display-column)
(scroll-view-lines! host view-id fractional-lines)
(set-caret-reveal! host reveal?)
(interaction-status host)
(interaction-provider host)
(interaction-origin-project host)
(refresh-interaction! host)
(submit-interaction! host)
(interaction-history host history-name)
(set-interaction-history! host history-name entries)
(select-interaction-candidate! host zero-based-index)
(set-interaction-history-position! host index-or-#f draft input)
(set-message! host message)
(project-index-state host project-id)
(request-project-index! host project-id)
(normalize-resource-path host path)
(set-buffer-resource! host buffer-id path)
(rename-buffer! host buffer-id name)
(buffer-id-by-resource host path)
(resource-mode host path)
(project-for-resource host path)
(project-provider-definitions host)
(project-id-by-root host root)
(create-project! host name roots provider marker)
(set-buffer-project! host buffer-id project-id-or-#f)
(begin-buffer-save! host buffer-id)
(complete-buffer-save! host buffer-id)
(abort-buffer-save! host buffer-id)
(open-buffer-ids host)
(create-buffer! host name initial-text kind resource-or-#f read-only? mode-or-#f
                cpp-indent-style-or-#f style-origin)
(buffer-saving? host buffer-id)
(buffer-modified? host buffer-id)
(release-buffer! host buffer-id replacement-buffer-id)
(exit-editor! host)
(split-window! host window-id axis)
(delete-window! host window-id)
(delete-other-windows! host window-id)
(open-window-ids host)
(active-window-id host)
(window-view-id host window-id)
(window-role host window-id)
(set-window-role! host window-id role-or-#f)
(window-pinned? host window-id)
(set-window-pinned! host window-id pinned?)
(window-created-by-policy? host window-id)
(workbench-slot host workbench-id role)
(focus-window! host window-id)
(request-redraw! host)
(%start-async-task! host request completed failed-or-#f cancelled-or-#f)
(%cancel-async-task! host task-id)
(%async-task-summaries host)
```

`(cind async)` wraps the normalized native task procedures:

```scheme
(async-file-read path)
(async-file-write path contents)
(async-directory-list path [maximum-entries])
(async-clang-format-style path fallback-preset fallback-origin)
(async-project-discovery path provider-definitions)
(async-rg-result-parse project-root output)
(async-process executable arguments [working-directory])
(start-async-task! host request completed
                   #:key (failed #f) (cancelled #f))
(cancel-async-task! host task-id)
(async-task-summaries host)
```

`start-async-task!` returns a positive application-local task ID. `completed` receives the ID and a
typed result. File reads produce `#(file-read path exists? contents)` and atomic writes produce
`#(file-write path)`. Directory enumeration produces
`#(directory-list normalized-path entries)`, where each entry is `#(path name directory?)`.
Clang-format discovery applies the Scheme-selected fallback preset when no configuration exists
and produces `#(clang-format-style path found? cpp-indent-style origin)`. Project discovery produces
`#(project-discovery path root-or-#f provider-or-#f marker-or-#f)` from the immutable provider
definitions supplied with the request. Processes produce
`#(process exit-status termination-signal standard-output standard-error)`. A nonzero process exit
status remains a completed result; Scheme policy interprets tool-specific statuses. Ripgrep result
parsing produces `#(rg-result-parse text locations)`, where every location is
`#(source-start source-end resource zero-based-line zero-based-byte-column)`.

`failed` receives the task ID and native error string. Without a failure procedure, the error is
retained as the runtime's latest scripting diagnostic. `cancelled` receives the task ID after the
native operation acknowledges cancellation. `cancel-async-task!` reports whether a live native
task accepted the request. `async-task-summaries` returns outstanding `#(id kind)` records for
introspection.

All three terminal callbacks run on the editor thread after the frontend drains the native runtime.
The bridge protects their procedures while the task is outstanding and removes the task before
invocation. Scheme code never runs on the libuv loop or worker pool.

`define-command!` registers a Scheme procedure in `CommandRegistry`. `execute` receives an
immutable command context and invocation value. `enabled` receives the same context and is either a
predicate or `#f`. The runtime protects registered procedures from collection and invalidates them
with their owning application. A command definition records documentation and its native, bundled
Scheme, or extension source. `set-command-documentation!` supplies an explicit docstring when the
procedure itself has none. `command-properties` returns the name, optional documentation, source,
enabled state, and bindings resolved from the active keymap stack.

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

`keymap-context-snapshot` returns the focused Buffer kind, the named keymaps attached to the
InputState stack, Window, View, Buffer, active minor modes, and major mode, and whether display
policy created the Window. It exposes attachment and activation facts without assigning precedence
or selecting global roots.

`(cind command)` owns the corresponding policy interface:

```scheme
(configure-keymap-policy! host
  #:editor editor-root-names
  #:application application-root-names
  #:overrides override-root-names
  #:policy-created-window policy-created-window-map-names)
(resolve-base-keymap-policy host context)
(resolve-keymap-policy host context)
(base-keymap-layers host context)
(active-keymap-layers host context)
(configure-modeline-policy! host procedure)
(resolve-modeline-content host context facts)
(configure-chrome-policy! host procedure)
(resolve-chrome-content host context facts)
(configure-theme-policy! host procedure)
(resolve-presentation-theme host)
(configure-style-policy! host procedure)
(resolve-presentation-styles host)
(configure-motion-policy! host procedure)
(resolve-presentation-motion host)
(configure-metrics-policy! host procedure)
(resolve-presentation-metrics host)
(configure-typography-policy! host procedure)
(resolve-presentation-typography host)
(resolve-presentation-profile host)
```

The default policy orders Window, View, Buffer, active minor-mode, major-mode, editor, and
application maps. The active policy prepends the InputState stack and supplies the configured
always-active override maps. Minibuffer contexts omit editor roots. Policy results carry keymap
names and diagnostic scopes; the native boundary validates every name and converts it to a
`KeymapId` before updating the command loop. `base-keymap-layers` and `active-keymap-layers` expose
name-only projections for scripted translators and self-description.

A modeline policy receives the explicit host, command context, and
`#(modeline-facts buffer-name resource-or-#f dirty? line column line-count revision style-origin
last-key input-state)` vector. It returns `#(modeline segments)`, where each segment is
`#(modeline-segment group tone weight debug? text)`. Groups are `chip`, `left`, and `right`; tones
are `strong`, `normal`, `faded`, `faint`, `salient`, and `critical`; weights are `regular` and
`strong`. `configure-modeline-policy!` replaces this procedure for one application. The native
boundary rejects malformed or empty segments before they enter the frontend-independent Scene.

The chrome policy receives immutable presentation facts:

```scheme
#(chrome-facts kind prompt input input-caret candidates selection-or-#f
               message preedit pending-sequence pending-prefix hints prompt-byte-count)
```

`kind` is `none`, `text`, or `picker`; candidates are `#(chrome-item label detail)` values and
hints are `#(chrome-hint key detail prefix?)` values. The policy returns:

```scheme
#(chrome pending-key echo echo-caret-or-#f popup-title items capacity
         selection-or-#f popup-input-or-#f popup-input-caret-or-#f)
```

Caret positions are UTF-8 byte offsets. Picker ownership of the minibuffer input, empty-result
layout, and which-key presentation are identical in terminal and pixel frontends.
`configure-chrome-policy!` replaces the procedure for one application; C++ validates byte offsets
and popup indices and projects the result into the shared Scene.

The theme policy returns a semantic ARGB palette shared by terminal and pixel presenters:

```scheme
#(presentation-theme canvas highlight band selection divider
                     text strong faded faint salient popout critical cursor
                     sign-added sign-modified sign-deleted)
```

Each color is an unsigned 32-bit straight-alpha ARGB value. Scheme owns the palette and uses it to
resolve the style policy; pixel and terminal presenters only encode the resulting attributes.

The style policy receives the resolved theme and returns concrete text attributes:

```scheme
#(presentation-styles inactive-alpha secondary-alpha
  #(#(presentation-style role foreground background-or-#f regular-or-strong) ...)
  #(modeline-strong modeline-normal modeline-faded
    modeline-faint modeline-salient modeline-critical))
```

The text-style vector contains every published presentation role exactly once. It covers Scene
style classes, popup subparts, echo key text, and active and inactive modeline text. The native
boundary rejects missing, duplicate, and unknown roles. Surface-bearing status, popup, position
hint, selection, and inactive-modeline roles require a background; other roles may use `#f`. Both
pixel and terminal presenters consume the same resolved foreground, background, weight, inactive
alpha, and secondary alpha. Terminal output composites alpha onto the effective background before
emitting true-color SGR.

`resolve-presentation-profile` resolves the theme once and supplies that exact value to the style
policy, then returns theme, styles, motion, metrics, and typography as one immutable frontend
snapshot. Native frontends acquire this profile atomically during application initialization.

The GUI motion policy returns
`#(presentation-motion view-duration-ms scroll-spring-frequency position-tolerance
velocity-tolerance)`. The duration is a positive integer and the remaining values are positive
finite reals. C++ owns interpolation, spring integration, caret constraints, and damage tracking;
Scheme owns their user-facing timing and response parameters.

The GUI metrics policy returns:

```scheme
#(presentation-metrics modeline-extra-height echo-extra-height footer-padding-x
                       segment-gap chip-padding-x minibuffer-padding-x
                       minibuffer-detail-gap cursor-stroke minimum-columns minimum-rows)
```

Pixel measurements are finite non-negative reals; visible padding and cursor thickness are
positive. Minimum dimensions are positive integers. Scheme owns the chrome proportions and minimum
window grid, while C++ derives font-relative rectangles, shaped-text positions, hit targets, damage,
and inspector geometry from the profile. The terminal frontend continues to use cell geometry.

The typography policy returns `#(presentation-typography font-family font-size)`. The family is a
non-empty generic or concrete font family and the size is a positive finite logical-pixel value.
Pixel frontends use this policy unless an explicit launch argument overrides one field. Font
resolution, fallback shaping, and rasterization remain presenter mechanisms.

`key-sequence-completions` performs a side-effect-free layered query over an explicit ordered
keymap vector; an empty sequence requests root entries. It returns `#(key detail prefix?)` vectors,
merges duplicate keys by layer precedence, and applies the same one-pass remap as resolution. These
operations let a transient translator inspect the command surface beneath itself without depending
on an implicit focused application.

`(cind input)` exposes the state-definition interface:

```scheme
(define-input-state! host name
  #:keymaps keymap-vector
  #:text-input 'accept
  #:cursor 'beam
  #:indicator ""
  #:handler #f
  #:on-enter #f
  #:on-exit #f
  #:position-hints #f)
```

The procedure creates or reconfigures a named state. Keymaps are an ordered vector of keymap names;
text input is `accept` or `ignore`; cursor shape is `beam`, `block`, or `underline`; and the
indicator is presentation text. The defaults describe the Emacs state. `%define-input-state!`,
`set-input-state-lifecycle!`, and `set-input-state-position-hints!` form its normalized native
boundary.

An optional handler receives a complete command context and canonical key notation. It returns
`pass`, `consume`, `#(dispatch command-name)`,
`#(dispatch command-name arguments)`, or `#(pending sequence hints)`. Dispatch arguments are typed
command values, and a dispatch without an explicit prefix inherits the command loop's pending
count, register, and extras. Pending consumes the key and publishes the supplied
`#(key detail prefix?)` hint vector through the same frontend-independent popup channel as keymap
prefixes. Handler errors are retained by the scripting runtime and consume the key without escaping
into a frontend event loop. The system override map is resolved before a handler, so `C-g` remains
an unconditional escape path.

Lifecycle procedures receive the same
`#(kind view-id from-state-or-#f to-state-or-#f)` value as state-change observers. `on-enter` runs
after the state joins a View stack; `on-exit` runs after pop, durable-state replacement, or View
release removes it. Lifecycle is stack membership, so pushing another transient state does not end
the obscured state's session. Conditions become scripting diagnostics and cannot roll back the
completed transition. Session ownership and cleanup therefore stay with the state definition
throughout its stack-membership lifetime.

`set-input-state-position-hints!` attaches an optional document-decoration query to an InputState.
The provider receives a command context and returns a vector of `#(byte-offset label)` values.
Only the top state contributes hints, so a transient state naturally obscures the durable state's
decorations. Providers are pure queries over the context's document snapshot, complete Selection,
effective mode policy, and InputState. The application memoizes a validated result for that tuple;
document edits, Selection changes, mode-policy changes, and state transitions derive a new result.
Offsets must be inside the document and labels must be non-empty. Provider conditions and malformed
results become inspection-visible errors rather than escaping through a frontend render loop.
Position hints enter the frontend-independent Scene as replacement decorations. Their explicit cell
span covers the source grapheme without changing document shaping, cursor placement, or line layout.

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

`(cind input)` also defines the shared `input.read-key` transient state and exposes
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

`define-language-profile!` composes named native providers by facet and supplies language-scoped
setting defaults. Provider associations use the facets `lexing`, `syntax`, `indentation`,
`structural-editing`, `highlighting`, `completion`, and `formatting`. A declaration is validated in
full before it replaces an existing profile. Each provider names an executable native mechanism,
not a capability flag. An editing session resolves the provider selected by its buffer's current
profile and creates mechanism state lazily. Facets backed by the same mechanism share that state,
including its incremental analysis cache.

The terminal indentation, fixture, and REPL tools install the same bundled mode policy before
selecting `cind.cpp`; they do not maintain a native fallback mode definition. C++ registers only
the C-family provider inventory and the dialect setting consumed by the Scheme profile.

`(cind core)` wraps `%define-mode!` as keyword procedures `define-major-mode!` and
`define-minor-mode!`. A major-mode definition accepts `#:parent`, `#:language`, `#:keymap`,
`#:interaction-class`, `#:initial-state`, and `#:things`; thing bindings are an association list of
semantic names to named Thing definitions. Parent modes have the same major/minor kind. When a
child keymap has no explicit parent, mode inheritance assigns the nearest parent mode keymap.
`mode-properties` returns the declared metadata, language profile, and effective keymap names.
`buffer-language-facet?` reports whether the active major-mode language profile binds a provider
for the requested facet.

`set-buffer-major-mode!` and `set-buffer-minor-mode!` mutate buffer-scoped mode state.
`buffer-mode-policy` returns `#(interaction-class initial-state things)`. Effective policy changes
notify procedures registered through `observe-mode-policy-changes!` with
`#(kind buffer-id mode-name-or-#f before-policy after-policy)`. A mode's explicit initial state
precedes the class mapping, and the most recently enabled minor-mode declaration precedes the major
mode. A change to the effective interaction class or initial state rederives every View of the
Buffer through that View's selected strategy while preserving its transient stack. Changes confined
to semantic thing bindings preserve the View's durable and transient input states.

`define-file-mode-rule!` registers a named declarative matcher containing a major mode, filename
suffixes and exact filenames. Matching is case-sensitive and the most recently defined matching
rule wins. Redefining a name replaces that rule and moves it to the highest precedence. File-backed
buffers resolve these rules when they are created or receive a resource; unmatched files and
scratch buffers use `fundamental-mode`. The bundled policy maps C-family suffixes to `cind.cpp` and
Scheme suffixes to `scheme-mode`. `scheme-mode` derives from `prog-mode` and adds `C-c C-e`,
`C-c C-r`, and `C-c C-b` for expression, region, and buffer evaluation. It uses plain-text input,
newline, deletion and rendering semantics until a Scheme language profile supplies corresponding
syntax, indentation and structural-editing facets. Commands consult those facets instead of
implicitly applying the C++ analyzer to a language-less mode.

The default self-insert and deletion commands query `structural-editing` before selecting
`type-text!` or structural `delete-grapheme!`; otherwise they compose `insert-text!` and raw
grapheme deletion. The structural native mechanisms reject a View whose active language profile
does not provide that facet.

`define-project-provider!` registers a named ordered set of single-component project markers.
`project-provider-definitions` returns an immutable Scheme value suitable for
`async-project-discovery`. Native worker code searches that supplied snapshot and returns the
provider, marker and root without entering Guile. At one directory, the most recently defined
provider has precedence, while a marker in a closer ancestor always wins over a farther one.

`define-interaction-provider!` registers an editor-thread Scheme completion procedure. The
procedure receives an immutable command context and query string and returns a vector of
four-element candidate vectors containing value, label, detail and filter text in the order the
picker should display them. The runtime
protects the procedure from collection, validates the complete result and invalidates the callback
with its application. Scheme conditions and malformed candidates become interaction errors and are
retained in the scripting inspection snapshot.

`enabled-command-names`, `open-buffer-summaries`, `project-root`, `project-files` and
`active-key-bindings` expose immutable registry and application snapshots. Path operations provide
platform-native relative, filename and parent calculations. These capabilities keep candidate
selection and presentation in Scheme while C++ retains registry identity and filesystem syntax.

`buffer-mode-summary` returns the current major mode, enabled minor modes, and effective policy;
`mode-properties` describes each registered definition. `owned-user-modules` returns only the
fresh extension modules and persistent evaluation module retained by the owning `GuileRuntime`, so
module inspection preserves application isolation.

`buffer-id-by-name` resolves a buffer name to its generational ID or `#f`. `display-buffer!` assigns
that buffer to the target window through the application view lifecycle. `move-caret-to-line!`
moves a view caret using zero-based logical line and display-column coordinates, clamps the line to
the document, resets vertical-motion state and requests caret reveal. `set-message!` replaces the
application's transient message. The message remains visible until the next normalized key begins
a new message lifetime. When it is empty, `(cind core)` derives idle echo text from the enabled
commands and their bindings in the active keymap policy. Mutating capabilities execute
synchronously on the owning editor thread and raise a Scheme condition when the ID or requested
transition is invalid.

`buffer-locations` exposes the semantic location vector attached to a generated buffer.
`set-buffer-locations!` validates ordered source ranges against the current document and atomically
replaces that vector. Location-list navigation remains a native presentation mechanism; Scheme
chooses when a generated buffer becomes the current navigation source.

`display-generated-buffer!` creates or replaces a named read-only generated buffer, applies the
major mode and presentation origin selected by Scheme, resets its cached View to the beginning, and
displays it through the same Window and View lifecycle as any other buffer. Help and evaluation
results are frontend-independent editor state rather than renderer-owned overlays.

## Interactive evaluation

Each `GuileRuntime` owns a persistent fresh user module for editor-initiated evaluation. The module
imports `(cind host)`, `(cind command)`, and `(cind input)`, and binds the application's capability
as `host`. It is created on first use, protected for the runtime lifetime, and released before the
host capability expires. Definitions are therefore shared by expression, region, and buffer
evaluation within one application without becoming process-global or visible to another editor.

`evaluate-scheme!` reads the complete source before evaluating it, associates reader locations with
the supplied source name, evaluates forms in order, and returns a typed result containing rendered
values, standard output, standard error, or a caught Scheme condition. Registry definitions made
during evaluation record the source name and advance the same installation revisions as extension
loading. The evaluation module is included in Scheme function and variable introspection after it
is created.

`scheme.eval-expression` uses the ordinary text interaction and history infrastructure;
`scheme.eval-region` evaluates the primary active range; and `scheme.eval-buffer` reads the current
Buffer snapshot. Single-line values use the message area. Stream output, multiple values, and
buffer evaluation use the reusable `*Scheme Evaluation*` generated buffer.

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
index and metadata intact. `soft-kill-range` is a non-mutating range mechanism. Scheme selects
`plain` line-end semantics or `structural` syntax-aware semantics after querying the Buffer's
language facets; an unavailable structural provider is rejected at the native boundary.

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
`normalize-resource-path` applies native filesystem path rules. `set-buffer-resource!`,
`rename-buffer!`, `set-buffer-major-mode!` and `set-buffer-project!` mutate one explicit Buffer
property. `buffer-id-by-resource`, `resource-mode` and `project-for-resource` query the registries
without changing editor state. The bundled save-as policy composes these mechanisms in Scheme.
`begin-buffer-save!` acquires a Buffer save lease and returns the immutable snapshot contents.
`complete-buffer-save!` publishes that snapshot as the saved baseline, releases the lease and
reports whether newer edits remain. `abort-buffer-save!` releases a failed or cancelled lease.
The bundled Scheme command chooses the destination, schedules `async-file-write`, owns completion
and failure feedback and closes the lease from the corresponding callback.

`open-buffer-ids` returns the application-owned buffers in lifecycle order. `buffer-saving?` and
`buffer-modified?` expose lifecycle state without deciding how commands respond to it.
`create-buffer!` atomically creates an undisplayed scratch, file, generated, or process buffer from
explicit resource, mode, style and presentation data. `release-buffer!` requires a distinct open
replacement, redirects every Window displaying the
released buffer, destroys its Views, and removes the Buffer as one native lifecycle operation. It
returns `#f` on success or an invariant error string. The bundled Scheme policy refuses an active
save, applies the force/modified rule, and asks the configured fallback-buffer policy for a
replacement when no other Buffer remains. The default fallback policy creates a writable
`*scratch*` Buffer in `fundamental-mode`.

`(cind lifecycle)` resolves application bootstrap through a per-host startup procedure configured
with `configure-startup-policy!`. The procedure receives
`#(startup-facts requested-resource has-initial-text?)` and returns:

```scheme
#(startup-plan
  #(startup-buffer name contents kind resource-or-#f read-only? mode)
  cpp-indent-style-or-#f
  style-origin
  resource-to-open-or-#f
  startup-placeholder?)
```

`contents` is `initial-text` or `empty`. A false style selects the native C-family mechanism's
baseline style; `style-origin` is the user-facing provenance shown by presentation policy. C++
validates the complete plan against the Mode registry, creates the Buffer, View and Window, and
supplies initial text only when requested by the plan. A deferred resource is passed to the shared
Scheme `open-resource!` policy. The lifecycle module
retains the placeholder Buffer identity in per-host Scheme state so successful asynchronous open
policy can release it. `configure-fallback-buffer-policy!` replaces the procedure used when a
command needs a Buffer after releasing the last open Buffer.

Standalone editor sessions use a separate per-host procedure configured with
`configure-session-policy!`. The procedure receives `#(session-facts has-initial-text?)` and
returns:

```scheme
#(session-plan
  #(startup-buffer name contents kind resource-or-#f read-only? mode))
```

The native session mechanism validates this plan through the same startup-buffer contract and
constructs the requested Buffer and View around the supplied text. This contract is used by
headless editing sessions and fixture-driven editor operations, allowing Scheme to select their
buffer identity, kind, resource, mutability and major mode.

`configure-close-policy!` maps a normalized close request and its `force?` flag to a registered
command name. GUI window-close and terminal EOF paths submit the same request through
`EditorApplication`; C++ validates the selected command against the registry and executes it
through the normal command loop. The default policy selects `application.quit` for an ordinary
request and `application.force-quit` for a forced request, so confirmation and modified-buffer
behavior remain ordinary Scheme command policy.

`(cind pointer)` dispatches normalized semantic hits through a per-host procedure configured with
`configure-pointer-policy!`. The procedure receives the active command context and:

```scheme
#(pointer-event kind window-or-#f line-or-#f column-or-#f popup-item-or-#f pending-key?)
```

GUI code resolves pixels to a Scene hit and converts the hit into this frontend-independent event.
The default policy ignores document clicks while an interaction or key prefix is active, focuses
the target Window, and composes `window-view-id` with `move-caret-to-line!`. Gutter hits select
display column zero. The host operations own validated Window/View lookup and caret mutation; the
policy owns when and how a semantic target invokes them.

`configure-scroll-policy!` receives a normalized input fact:

```scheme
#(scroll-input amount lines-or-steps)
```

Platform frontends preserve precise trackpad movement as fractional `lines` and identify discrete
wheel movement as `steps`. The default policy maps each step to three lines and composes
`scroll-view-lines!` with `set-caret-reveal!`: the first operation clamps and updates only the
target Viewport, while the second controls whether layout follows and paints the caret. Frontends
own native event interpretation; Scheme owns editor scroll speed and behavior.

Window capabilities operate on explicit generational IDs. `split-window!` accepts `rows` or
`columns`; split, delete, retain and focus operations return `#f` on success or an invariant error
string. `open-window-ids` returns layout order, `active-window-id` identifies the current focus,
and `window-view-id` resolves presentation after a buffer is displayed. The bundled Scheme policy
uses these mechanisms for window cycling and asynchronous open targeting. `exit-editor!`
marks the native event loop for termination, and `request-redraw!` requests caret reveal. Scheme
commands inspect buffer state and own the quit confirmation interaction before invoking the exit
mechanism.

Workbench capabilities expose durable editing surfaces without transferring Buffer or Project
ownership. `workbench-list` returns `#(id name active?)` summaries; scope and MRU queries return
generational Project and Buffer IDs. The scoped Buffer queries optionally widen to the global
Buffer pool. Lifecycle mutations validate names and registry membership in native code while the
bundled commands own picker interaction and feedback. Window roles, pinning, policy provenance and
named slots are explicit mechanisms used by the Scheme display policy.

`workbench-session-state` returns the versioned serialization of every workbench, and
`restore-workbench-session!` validates and installs serialized state before resource loading
continues through the asynchronous runtime. The bundled `workbench.save-session` and
`workbench.restore-session` commands select a path through the minibuffer, compose
`async-file-write` or `async-file-read`, cancel a superseded restore request, and report completion
through the ordinary message area. Session state contains stable Project roots and Buffer resource
paths rather than runtime IDs.

`project-id-by-root` and `create-project!` expose registry identity and validated construction;
`set-buffer-project!` applies the chosen attachment. `project-index-state` returns
`#(revision indexing? error-or-#f)`, while `request-project-index!` submits an explicit refresh to
the native scan/watch mechanism. `(cind core)` requests an initial index only when the project has
no published revision and no scan in progress. It handles index-update events by refreshing a live
`project-files` interaction whose origin belongs to that project. `(cind core)` implements
`open-resource!` by
normalizing and deduplicating the path, snapshotting mode and provider policy, scheduling file,
clang-format and project-discovery tasks, and composing their results into buffer creation,
project attachment, window presentation, startup-placeholder cleanup and user feedback. Duplicate
opens update the pending target and position. The C++ `GuileRuntime::open_resource` entry point
invokes the same Scheme policy for deferred startup plans and native callers.

The bundled project-search policy starts `rg` with an explicit argument vector, interprets its exit
status, schedules typed result parsing, creates the read-only location-list buffer, installs its
semantic locations, attaches the project, selects a live target window and reports feedback.
Pending search records are scoped to the host and starting another search cancels the preceding
process or parser task. Native code provides process execution, parsing, validated registries and
presentation mechanisms without encoding this command policy.

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

Interactive commands import `(cind minibuffer)` and return one of its high-level constructors:

```scheme
(read-from-minibuffer "Project search: " "project.search.accept"
                      #:history "project-search")

(completing-read "Switch buffer: " "buffers" "buffer.switch.accept"
                 #:history "buffers")
```

Both constructors accept `#:initial-input`, `#:history`, `#:buffer-name`, and `#:arguments`.
`completing-read` also accepts `#:allow-custom-input?`; otherwise submission requires a provider
candidate. `arguments` is a proper list of typed values placed before the accepted string in the
accept command's invocation. The non-empty buffer name identifies the ephemeral native
`minibuffer` Buffer and defaults to `" *minibuffer*"`. Prompts, provider names, history names,
buffer names, and accept commands are explicit strings, so a request is independent of dynamically
scoped editor state. `interaction` remains the lower-level tag constructor used by
`(cind minibuffer)` and by code implementing another interaction policy.

`configure-minibuffer-history-policy!` installs a per-application procedure that receives the
current string vector and a submitted value and returns the replacement string vector.
`make-bounded-history-policy` constructs the default adjacent-deduplicating policy; the core policy
uses a capacity of 100. `(cind minibuffer)` implements previous/next history traversal, draft
restoration, and wrapping candidate movement from the `interaction-status` snapshot. Native code
stores named string vectors, applies absolute candidate indices, replaces minibuffer input, and
tracks the navigation position for inspection. The status vector is
`#(active? picker? has-history? history-or-#f selected-or-#f candidate-count
history-index-or-#f history-draft)`.

`(cind core)` defines the command palette and its dispatching accept command, file-open and save-as
interactions, named and relative buffer switching, buffer kill policy, goto-line parsing and
movement, window layout and application quit policy, project file selection and project search with
project-aware enabled predicates, key-help selection and message presentation, mark/region
commands, structural expression/list movement, and kill/yank behavior. Each installed core policy
owns a bounded kill ring in its Scheme closure. Copy and kill commands synchronize the newest entry
with the platform clipboard and also write the invocation's named register when present. A yank
with a named register reads that register; an unnamed yank reads the newest kill and imports the
clipboard when the ring is empty. File and project commands retain host-scoped asynchronous policy
records while composing native storage, indexing, process and parsing mechanisms.

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

`(cind core)` declares `fundamental-mode`, `prog-mode`, `scheme-mode`, and `special-mode`.
Fundamental and programming buffers have the `editing` interaction class; special buffers have
`interface`. The built-in C++ and Scheme modes derive from `prog-mode`, and generated location-list
buffers derive from `special-mode`. The default Emacs strategy maps both classes to the `emacs`
InputState.
`C-u` pushes the `emacs-universal` transient handler. Its state-local session owns the accumulation
phase while the shared command prefix carries the resulting count. Repeated `C-u`, decimal digits,
and `-` update that value; the first ordinary key pops the state and receives the prefix through its
`CommandInvocation`. `C-g` cancels both, while Backspace and Delete dispatch the raw deletion escape
commands from the same transient policy. An unbound printable key carries the prefix into the paired
text commit; the normalized text-input path consumes it and repeats the committed UTF-8 text for a
positive count.

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

`interaction-provider-task` from `(cind command)` constructs an asynchronous provider result from
an `(cind async)` request and a one-argument result transform. The transform returns the same vector
of four-string `interaction-candidate` values accepted from an immediate provider.

`(cind core)` defines the `commands`, `buffers`, `files`, `project-files`, `key-bindings`,
`scheme-functions`, and `scheme-variables` providers. Scheme filters internal commands and formats
semantic candidate fields from native snapshots. The `files` provider returns an
`interaction-provider-task` containing a typed directory-list request and an editor-thread result
transform; native code performs only the cancellable enumeration. `(cind minibuffer)` exports
`rank-completion-candidates`, whose default policy performs case-folded multi-term substring
matching and stable score ordering, and `rank-provider-result`, which applies the same policy to
immediate results or an asynchronous result transform. The bundled provider registrations compose
that policy explicitly. `InteractionController` preserves provider order and owns only absolute
selection state, generation-safe completion, cancellation, named history storage, and minibuffer
object lifetimes.

## Editor self-description

`(cind introspect)` implements `describe-key`, `describe-command`, `describe-bindings`,
`describe-mode`, `describe-function`, and `describe-variable` policy over host snapshots. Key
description uses the shared `input.read-key` state and layered keymap resolution, including named
prefix maps and remaps. Command and mode descriptions consume registry metadata. Function and
variable descriptions enumerate local bindings from bundled modules and the isolated extension
modules owned by the application.

All descriptions render into the reusable `*Help*` buffer. Values and source forms are bounded
before formatting, and a missing or changed binding produces a command error instead of retaining a
raw module pointer in interaction state. The command palette, Help providers, GUI inspector, and an
Ares lookup client can therefore consume the same authoritative identities and metadata.

Scripted providers run their preparation and result transforms on the editor thread. Blocking work
is expressed as a typed native request and never enters Guile. The Guile async bridge owns both
Scheme callbacks and provider transforms while their tasks are outstanding, so shutdown,
inspection, cancellation, and extension rollback observe one task namespace.

Additional host APIs follow the same boundary:

- pass stable names, generational IDs, immutable snapshots and typed values across languages;
- mutate editor state only on the editor thread;
- return command actions and interaction requests as data;
- schedule blocking work through `AsyncRuntime` and apply completions on the editor thread;
- keep frontend, Skia, SDL and terminal objects outside the scripting ABI.

## Ares development runtime

[Guile Ares RS](https://git.sr.ht/~abcdw/guile-ares-rs) fits the development backend for Scheme
source. Ares is a Guile library and nREPL-compatible RPC server; the `rs` suffix means RPC Server.
Its protocol provides interruptible asynchronous evaluation, streamed output, stdin, completion,
symbol lookup, documentation, arglists, sessions and backtraces.

CMake builds Guile 3.0.11 into a private build-tree prefix and links `GuileRuntime` against that
library. Fibers 1.4.3 is built by the same dependency graph against the private Guile and installed
into the same prefix. The Ares 0.9.7 Scheme sources are vendored under
`third_party/guile-ares-rs`; the embedded runtime adds that source directory and the private Fibers
source and compiled-module directories to its module search paths. This keeps the executable,
native Fibers extension and Scheme modules on one Guile ABI.

Ares protocol results map onto existing editor mechanisms:

| Ares/nREPL result | cind mechanism |
| --- | --- |
| completion candidates | interaction provider |
| definition file, line and column | source location and location navigation |
| evaluation values and streamed output | generated result buffer |
| backtrace frames | location-list buffer |
| interrupt and session lifecycle | cancellable asynchronous operation |

A standalone Ares process supplies project-wide Guile development without sharing editor memory.
The application endpoint uses the same Guile VM and persistent evaluation module as
`GuileRuntime`, which makes `(cind host)`, the explicit `host` capability, loaded cind policy
modules, and definitions created through `M-:` available for live inspection and evaluation. Ares
owns its evaluation threads and Fibers scheduler. Native editor mechanisms retain their
editor-thread checks, so evaluation threads cannot mutate editor state directly.

`scheme.ares-start` binds a random unprivileged port on the loopback interface and publishes it as
`.nrepl-port` in the active project root. `scheme.ares-status` reports the listener state and
address. `scheme.ares-stop` closes the listener, terminates the scheduler and removes the port file.
`GuileRuntime` performs the same stop operation during destruction. The controller is keyed by the
host capability, so multiple editor applications do not share endpoint state.
