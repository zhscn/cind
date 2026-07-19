# Workspace edits

A workspace edit is a set of immutable-snapshot text edits spanning one or more Buffers. Protocol
adapters and Scheme features resolve external resources and coordinates first, then submit
`WorkspaceBufferEdit` values to the shared executor.

## Validation and publication

Each Buffer edit declares its source revision. The executor groups repeated Buffer entries and
validates every participant before publishing any change:

- the Buffer exists, is writable, and still has the declared revision;
- each range is contained in that revision;
- ranges within a Buffer are non-overlapping;
- multiple insertions at one byte position are rejected as ambiguous.

The executor opens one pending transaction per Buffer and applies ranges from the end of the
document toward the beginning. It commits only after all pending transactions contain their final
text. A publication failure restores Buffers already committed before returning an error.

## Undo grouping

The result contains one `TransactionGroupEntry` for every changed Buffer. Feature policy records
these entries in the active Workbench's `TransactionGroupRegistry`, giving rename, code actions,
composed views, and future refactors one cross-Buffer undo unit while retaining each Buffer's native
branching undo tree.

Transaction-group undo is conflict-aware. A member is moved only when its current undo position
still matches the recorded edge; Buffers edited independently after the workspace operation are
reported as skipped rather than overwritten.
