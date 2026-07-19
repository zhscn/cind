# LSP sessions and semantic completion

cind runs language servers as project-scoped services. The native layer owns process transport,
JSON-RPC framing, request generations, document synchronization, protocol data conversion, and
session lifetime. Scheme policy selects the language server source used by a mode. Completion
candidates enter the same frontend-independent pipeline as local word and path candidates.

## Ownership

`LspSessionRegistry` owns one session for each project/root, language, and server configuration.
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
Text offsets are converted through the document's persistent UTF-16 summaries, so protocol
positions and editor byte offsets share the same revisioned snapshot without rescanning the file.

## Documents and requests

The first request for a file sends `textDocument/didOpen`. A later request at a new document
revision sends a full-content `textDocument/didChange`; releasing the buffer sends
`textDocument/didClose`. The synchronization table is session-local and records the last revision
published for each URI.

Completion requests use monotonically increasing JSON-RPC IDs. Cancellation removes the owning
callback and sends `$/cancelRequest` after a request has reached the server. Responses are delivered
only on the editor thread. Session failure completes every pending request with the same transport
error and leaves local completion providers usable.

The initialize handshake records `completionProvider.resolveProvider`. Servers that advertise it
produce unresolved completion items. `completionItem/resolve` uses the same request ownership and
cancellation path as initial completion, including transport failure and session shutdown.

## Completion integration

The default C++ policy requests the fixed `Word`, `Path`, and `Lsp` sources. Include syntax narrows
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

The GUI inspector exposes sessions at `editor.lsp`, including lifecycle state, command, project
root, pending request count, synchronized document count, resolve capability, and transport errors.
Completion state at `editor.completion` identifies LSP candidates by their fixed provider/session
name and exposes resolve progress, resolve errors, and normalized documentation.
