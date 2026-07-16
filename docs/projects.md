# Projects

A project is the editor's tooling, configuration and file-discovery boundary. Buffers refer to a
project by generational `ProjectId`; windows and views remain independent presentation state. A
project owns roots, project-scoped settings and an immutable file-index snapshot.

## Discovery and attachment

Opening a file searches its ancestor directories for `.git`, `cmk.yaml` or
`compile_commands.json`. The closest directory containing a marker becomes the project root. This
search runs with the file read and style discovery on the async worker pool. The editor reuses a
project with the same normalized root and assigns the opened buffer on the editor thread.

Project discovery is provider policy rather than a responsibility of `ProjectRegistry`. Native and
scripted discovery providers can therefore create the same validated `ProjectSpec` without giving
the registry filesystem access.

## File index

The project service recursively scans a root on the worker pool and publishes a sorted file list as
one main-thread update. Each update increments `index_revision`; `indexing` and `index_error`
describe the current refresh state. A scan is limited to 200,000 files and skips version-control
metadata, dependency caches and conventional build-output directories.

After publishing an index, the service retains libuv directory watches for the indexed directory
set. Existing watches survive refreshes, removed directories lose their watches, and newly indexed
directories receive new watches. Starting a new watch requests a verification scan, closing the
event gap between the preceding snapshot and native watcher activation. Multiple events while a
scan is active coalesce into one pending refresh. A project retains up to 4,096 directory watches;
the root and earlier indexed directories remain covered when a larger tree exceeds that limit.

## Commands

`project.find-file` (`C-x p f`) opens a picker backed by the current immutable index. Candidate
labels are relative to the project root, while submitted values are normalized absolute paths.
Index completion refreshes an open project-file picker without moving provider work onto the UI
thread.

`project.search` (`C-x p g`) prompts for a pattern and launches `rg` with the project root as its
working directory. The libuv process service captures stdout and stderr without a shell. Exit
statuses zero and one produce a read-only process buffer; other statuses surface the diagnostic in
the echo area. Starting another search cancels the previous application-wide project search.
