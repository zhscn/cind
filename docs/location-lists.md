# Location lists

A location list is a read-only generated buffer whose text ranges link to source locations. Search
results, compiler diagnostics, test failures and language-service references can share this model
without teaching frontends or the command loop about a particular producer.

## Buffer contract

`BufferLocation` maps a non-empty `source_range` in the generated document to a normalized resource
and zero-based `EncodedLinePosition`. The column declares either UTF-8 byte or UTF-16 code-unit
encoding, so producers can retain protocol coordinates while the target resource is unopened.
`BufferRegistry::set_locations` validates that source ranges are
ordered, non-overlapping and contained in the current document. The buffer exposes immutable
locations and a point lookup; navigation commands consume these semantic records rather than
reparsing display text.

Location data is separate from settings and mode definitions. Settings remain scalar policy,
while locations are revision-bound document data owned by a particular generated buffer.

## Major mode

`cind.location-list` is a language-less major mode derived from `special-mode`, with an
`interface` interaction class and a sparse local keymap:

- `M-n` moves to the next location.
- `M-p` moves to the previous location.
- `RET` opens the location at point in the current window.

Each Guile workbench retains one current location list independently of the buffer displayed in a
window. `M-g n` and `` C-x ` `` visit the next location, while `M-g p` visits the previous location.
These commands continue to use the retained list after a visit replaces the result buffer with a
source buffer. Selecting a location also moves the result View cached for that navigation window;
other windows retain independent View state. Navigation creates a hidden result View at the current
position when its target window has not displayed the list before.

A language-less major mode supplies an empty syntax-token stream to both GUI and TUI frontends.
The generated document therefore stays plain text and does not run C++ analysis merely
because it is displayed by an editor window. Global and application keymaps remain available
through the normal layered keymap pipeline.

Opening a location uses the same asynchronous file workflow as ordinary file opening. Existing
buffers are reused, pending opens retain the latest requested destination and its encoding, and the
final caret is converted to a UTF-8 byte position only after the target Buffer is available.

The native `LocationListStack` owns durable list identity, items, resolved Buffer anchors and
resource fallback positions. `(cind workbench)` owns publication order, the current-list reference,
the selected index and the association with a generated Buffer. `publish-location-list-data!`
atomically installs native data and returns the workbench, list identity, Buffer and item count;
Scheme records those facts as navigation policy. Killing the generated Buffer clears only its
association, so the durable list and its selected location remain navigable. Creating a new result
makes its list current without coupling navigation commands to a producer.

## Search producer

Scheme project-search policy invokes ripgrep with a NUL separator between each path and its line
record, keeping filenames containing `:` unambiguous. Process completion submits a typed parsing
request to the async worker pool. The native parser produces display text and `BufferLocation`
records as one immutable result; its Scheme callback creates the location-list buffer and installs
both on the editor thread.

Ripgrep exit status zero represents matches and status one represents an empty list. Tool failures
remain echo-area diagnostics and do not create a result buffer.

## Diagnostic producer

`diagnostic.list` reads revision-current diagnostics from the active Buffer and creates a generated
location list with severity, producer, optional code, and message. Entries use byte coordinates
because the Buffer diagnostic store has already resolved protocol positions against the current
document snapshot. `C-c l e` opens this list.
