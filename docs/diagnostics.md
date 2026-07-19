# Diagnostics

Diagnostics are revision-bound annotations owned by named producers. The core Buffer model stores
their ranges and metadata; language services, compilers, tests, and Scheme extensions decide when
to publish or clear a set. Frontends consume one shared model.

## Buffer model

`DiagnosticSet` contains an owner, a Buffer revision, and zero or more `Diagnostic` values. A
diagnostic records a UTF-8 byte range, severity, message, source, and optional code. Publishing
validates the owner, revision, range, and message. Replacing one owner's set leaves every other
producer intact.

Only sets matching the current Buffer revision participate in queries and presentation. An edit
therefore hides stale annotations without mutating producer state. Producers publish a replacement
for the new revision or explicitly clear their owner when the service is detached.

## Producers and presentation

LSP sessions publish under a session-specific owner after validating the document URI and version
and converting protocol UTF-16 positions against the target snapshot. Scheme extensions inspect
the merged current set with `buffer-diagnostics` and can construct higher-level tools without a
feature-specific C++ presentation path.

The editor gutter reserves one sign column shared with change signs. A diagnostic takes precedence
on its line, and the most severe diagnostic selects the glyph and style. `diagnostic.list` creates a
standard `cind.location-list` buffer for detailed messages and source navigation. Its default key is
`C-c l e`.

The GUI inspector reports `count`, `errors`, and `warnings` under the `diagnostics` member of each
`editor.buffers` entry. Gutter primitives use stable `line:N/diagnostic` identifiers and diagnostic
style classes, so rendering and model state can be inspected together.
