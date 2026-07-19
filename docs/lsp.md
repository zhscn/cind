# LSP sessions and features

cind runs language servers as project-scoped services. `LspSession` is the method-independent
transport boundary: it owns the process, JSON-RPC lifecycle, pending requests, cancellation,
capability exchange, document synchronization, and inbound message routing. Protocol features own
their method names, parameter construction, response decoding, and editor-facing data types.

The public feature boundary uses serialized JSON values rather than exposing the JSON library in
public headers. `LspRequest`, `LspResponse`, `LspResponseError`, `LspNotification`, and
`LspServerRequest` preserve protocol payloads while the session validates every outbound value.
Feature adapters decode only the messages they own.

## Ownership

`LspSessionRegistry` owns one session for each project/root, language, server configuration, and
set of client capability fragments.
A session owns a long-running process in `AsyncRuntime`; the process exposes writable stdin and
streams stdout and stderr back to the editor's owning thread. libuv callbacks never mutate editor
state directly.

The registry is destroyed before the asynchronous runtime. Completion requests are cancelled
before their session is destroyed, and runtime shutdown suppresses callbacks from processes that
are being terminated.

## Transport

`JsonRpcFramer` incrementally parses `Content-Length` framed messages. It accepts headers and bodies
split across arbitrary stdout chunks and multiple messages delivered in one chunk. Header and body
limits bound retained input.

The session performs the LSP initialize/initialized handshake and advertises UTF-16 positions.
Each enabled feature contributes a client capability JSON fragment. The session recursively merges
objects, unions array members, and rejects conflicting scalar declarations before starting the
server. Server capabilities remain available as serialized values through path lookup, with a
boolean convenience query for capability flags.

Inbound notifications and server requests are routed by method. A feature registers only the
methods it owns, so diagnostics, progress, configuration, and future protocol extensions do not
share a global switch. Unsupported server requests receive a method-not-found response. The
session provides the empty `workspace/configuration` response required by servers when no feature
owns that method.

## Documents and requests

`synchronize_document` is independent of any feature request. The first snapshot for a URI sends
`textDocument/didOpen`; a greater revision sends one full-content `textDocument/didChange`; closing
the buffer sends `textDocument/didClose`. Equal revisions are idempotent and stale revisions are
rejected. Changing the language identity closes and reopens the document. Snapshots queued during
initialization are coalesced to the newest revision before publication.

All feature requests use one monotonically increasing ID space and one pending-request table. A
request may be queued while the server initializes. Cancellation removes its callbacks and sends
`$/cancelRequest` when the request has reached the server. Structured JSON-RPC errors preserve the
numeric code, message, and optional data payload. Responses and inbound handlers run on the
editor's owning thread. Session failure completes every pending request with the transport error.

Notifications are sent through the same validated transport once initialization is complete.
Features that need a document synchronize its snapshot before issuing the associated request.

## Feature adapters

A feature adapter supplies three pieces:

- client capability fragments for `LspSessionConfig`;
- request and notification functions built on the generic session API;
- method-specific conversion between serialized protocol values and native editor values.

Adapters query server capabilities before using optional methods and register method-specific
handlers for server-initiated traffic. Adding definition, references, rename, hover, diagnostics,
or workspace symbols does not add pending maps or protocol branches to `LspSession`.

## Completion feature

`LspCompletionFeature` implements `textDocument/completion` and `completionItem/resolve` on the
generic feature boundary. It contributes completion and resolve client capabilities, synchronizes
the request snapshot, converts UTF-16 positions, and normalizes protocol items for the completion
pipeline. The initialize handshake's `completionProvider.resolveProvider` flag determines whether
items enter the pipeline as resolved.

The default Scheme policy requests the fixed `Word`, `Path`, and `Lsp` sources. Include syntax narrows
the request to `Path`; other string and comment contexts remain suppressed by the syntax gate.
Local candidates can open the menu immediately while the LSP request is pending. A semantic
response replaces only its own provider source and participates in the common fuzzy ranking.

LSP completion items retain distinct insert and replace ranges. `filterText`, `sortText`, kind,
detail, documentation, snippet metadata, and the provider payload are normalized once when the
response arrives. Main and additional text edits are converted against the request snapshot and
applied by the completion pipeline as one undo transaction.

The pipeline resolves the selected candidate and candidates in the visible menu window. Each item
has a stable pipeline ID; a resolve response replaces that item in place without resetting ranking
or selection. Query generations cancel outstanding resolves before reusing cached provider items.
Accepting an unresolved item queues its application behind the resolve request so additional edits
remain part of the same transaction. The selected item's documentation is presented in an adjacent
overlay when the viewport has room, without changing document or completion-menu geometry.

## Semantic navigation

`LspNavigationFeature` implements definition, declaration, implementation, and references requests.
It synchronizes the origin snapshot, encodes the caret as UTF-16, accepts both `Location` and
`LocationLink` responses, and normalizes local file URIs while retaining the declared UTF-16 range
encoding. Location lists and pending file opens preserve that encoding until the target Buffer is
loaded, then resolve it to the editor's UTF-8 byte coordinates. The shared LSP session contributes
all navigation capability fragments together with
completion capabilities, so one project and language retain one server process.

Scheme commands own result presentation. A single definition, declaration, or implementation opens
the target through the workbench display policy with the matching semantic intent. Multiple targets
and every references result become a `cind.location-list` buffer and the current workbench location
list. Visiting an entry uses the `list` intent. These display paths record `def`, `ref`, or `list`
edges in the workbench jump graph without coupling the LSP adapter to window placement.

`M-.` requests a definition, `M-?` requests references, and the `lsp.definition`,
`lsp.declaration`, `lsp.implementation`, and `lsp.references` commands remain available to other
keymaps and extensions. Starting another semantic navigation request cancels the pending request;
the editor-wide quit command uses the same cancellation path.

The GUI inspector exposes sessions at `editor.lsp`, including lifecycle state, command, project
root, generic pending request count, synchronized document count, complete server capabilities, and
transport errors.
Completion state at `editor.completion` identifies LSP candidates by their fixed provider/session
name and exposes resolve progress, resolve errors, and normalized documentation.

## Diagnostics

`LspDiagnosticsFeature` owns `textDocument/publishDiagnostics`. It validates notification payloads,
retains the server document version, and converts UTF-16 ranges only after resolving the target to
an open Buffer. Notifications for stale revisions, closed resources, and sessions that do not own
the Buffer are ignored.

The Buffer diagnostic store is keyed by producer. Each set belongs to one document revision, so
editing immediately excludes stale diagnostics while the synchronized language server computes a
new publication. Multiple producers can coexist without replacing one another. Severity, source,
code, message, and byte range remain available to Scheme through `buffer-diagnostics`.

The editor gutter shows the highest-severity diagnostic for each visible line in the existing sign
column. The `diagnostic.list` Scheme command builds a `cind.location-list` buffer from the active
diagnostics; `C-c l e` opens it and normal location-list navigation visits the source range. The GUI
inspector exposes diagnostic totals and error/warning counts for every entry in `editor.buffers`.
