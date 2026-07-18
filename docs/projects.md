# Projects

A project is the editor's tooling, configuration and file-discovery boundary. Buffers refer to a
project by generational `ProjectId`; windows and views remain independent presentation state. A
project owns roots, project-scoped settings and an immutable file-index snapshot.

## Discovery and attachment

Scheme registers named project providers as ordered marker sets. The bundled policy defines VCS
markers (`.git`, `.hg`, `.svn`), `cmk.yaml`, and `compile_commands.json`. Opening a file snapshots
the provider registry in Scheme and supplies that immutable value to the native asynchronous
discovery mechanism. The closest matching directory becomes the project root; at one directory,
the most recently registered provider has precedence. Scheme reuses or creates the matching
registry project, assigns the opened buffer and requests indexing on the editor thread.

`ProjectRegistry` validates and owns projects without filesystem access. A discovered project
retains its provider and matching marker as diagnostic metadata. Scheme changes declarative
provider policy through `define-project-provider!`; native code owns path normalization,
cancellation, filesystem queries, validated project identity and indexing mechanisms.

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

`project.search` (`C-x p g`) is Scheme policy that prompts for a pattern and launches `rg` with the
project root as its working directory. The libuv process mechanism captures stdout and stderr
without a shell. Exit statuses zero and one produce a read-only `cind.location-list` buffer; other
statuses surface the diagnostic in the echo area. A typed native task parses results on the worker
pool and returns generated text with semantic source locations. Scheme installs both values,
attaches the project and presents the buffer in the originating window when it remains live.
`M-n` and `M-p` move between results, and `RET` opens the result at its exact byte line and column.
Starting another search cancels the preceding host-scoped process or parser task.
