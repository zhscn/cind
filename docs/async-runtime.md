# Asynchronous runtime

cind keeps editor state single-writer while allowing file operations, filesystem monitoring and
child processes to run concurrently. `AsyncRuntime` owns a dedicated libuv loop thread and submits
blocking work to libuv's worker pool. Each `EditorApplication` owns one runtime for the lifetime of
its buffers, views, commands, projects and interactions.

```text
editor thread                  libuv loop                 worker pool
     │                             │                           │
     ├─ submit(task) ─────────────>│                           │
     │       uv_async_t            ├── uv_queue_work ─────────>│
     │                             │                           ├─ blocking work
     │                             │<──── after-work callback ─┤
     │<─ native event-loop wakeup ─┤                           │
     ├─ drain()                    │                           │
     └─ apply completion           │                           │
```

## Thread ownership

Worker callbacks receive immutable input and a `std::stop_token`. They return a completion callback
containing the result instead of mutating editor objects. The runtime places that completion in a
thread-safe ready queue and invokes the frontend's wakeup callback. The GUI posts an SDL user event;
the terminal frontend writes to a nonblocking pipe monitored together with standard input. Wakeup
callbacks are thread-safe, non-throwing notifications and never read or write editor state.

The frontend calls `drain()` from its event-loop thread. Completed, cancelled and failed callbacks
therefore run on the same thread as commands, buffer edits and scene composition. Rendering code
and editor registries do not require cross-thread locking.

Submissions and cancellation requests enter the libuv loop through a mutex-protected command
queue. Its async callback drains the complete queue because multiple `uv_async_send` calls may be
represented by one loop callback. Libuv handles and requests are created, queued, cancelled and
closed only on the loop thread.

Directory watches and child processes use the same command and ready queues. Watch callbacks,
process exit results, stdout, stderr and failures become immutable ready records before crossing
back to the editor thread. Persistent watches do not count as background work; a delivered watch
event and any indexing task it starts do.

## Task lifecycle

`submit()` returns an `AsyncTaskId`. A task remains outstanding until its main-thread callback has
been drained. `has_work()` includes queued work, running work and undrained results, which makes it
suitable for lifecycle checks without using it as an event-loop polling mechanism.

`cancel()` requests the task's standard stop source and asks the loop thread to cancel work that has
not started. Work that may already be running checks the token at safe points and throws
`AsyncTaskCancelled` only when abandoning the operation preserves its external invariants. A stop
request does not discard a completion that crossed an irreversible commit point.
Cancellation and failure callbacks follow the same main-thread delivery path as successful
completion. A true return value means that the task was still known and received a stop request; it
does not claim that worker execution was preempted.

Destroying the runtime requests cancellation, waits for active libuv requests, closes its async
handle, stops directory watches, terminates child processes and joins the loop thread. Ready
callbacks that have not been drained are discarded during shutdown. `EditorApplication` declares
the script adapter and runtime after the state captured by their callbacks. The adapter cancels its
tasks first, then runtime shutdown completes before buffers and registries are destroyed.

## Native handles

`watch_directory()` creates a non-recursive libuv filesystem watch and returns an
`AsyncWatchId`. Started, changed and failed callbacks run on the editor thread. `unwatch()` makes
already queued events inert and closes the native handle on the loop thread. Higher-level services
compose recursive monitoring by retaining one watch for each indexed directory.

`spawn()` starts a child process without a shell, captures stdout and stderr independently, and
returns an `AsyncProcessId`. A normal exit is a completed process even when its status is nonzero;
the caller owns tool-specific exit-code policy. `terminate()` requests process cancellation with
`SIGTERM`. Runtime shutdown uses `SIGKILL` so an uncooperative child cannot retain the application
lifetime. Captured output is bounded to 64 MiB per stream.

## Editor integration

File reads, directory enumeration, clang-format discovery, project discovery and atomic saves run
as worker tasks. Scheme open policy starts the applicable operations in parallel and replaces the
temporary startup buffer after their typed results are ready. Project discovery consumes the
immutable provider value supplied with its request, so worker tasks never call Guile or read
mutable editor state.
File save captures an immutable document snapshot and
performs the atomic file replacement as one worker task. Its completion marks that snapshot as the
save point. Edits made while the write is in progress stay modified, so asynchronous completion
cannot mark newer content as saved.

Project indexing uses worker tasks, directory watches and generation checks. The native service
publishes immutable registry snapshots and emits an index-updated event; Scheme decides when an
initial index is needed and which interaction should refresh in response. Scheme project-search
policy runs `rg` through the process service, interprets tool exit status and schedules the typed
ripgrep parser. Native parsing runs on the worker pool; its Scheme completion creates and presents
the read-only location-list buffer on the editor thread.

Modes and services can submit additional work through `EditorApplication::async_runtime()`. Their
completion callbacks must validate any resource identity or revision they captured before applying
results. Long-running work should check its cancellation token between independently safe units of
work.

## Script tasks

`AsyncScriptHost` gives embedded languages one task namespace over native file reads and writes,
directory enumeration, clang-format discovery, project discovery, ripgrep result parsing and child
processes. Each request receives a stable integer ID and follows the same completed, cancelled or
failed terminal path. The adapter retains the native `AsyncTaskId` or
`AsyncProcessId` internally, so cancellation does not expose libuv handle types to language code.

File and style workers receive copied paths and immutable read or write payloads. Directory workers
receive copied paths and limits. Project discovery receives a copied ordered provider snapshot.
The ripgrep parser receives a copied root and captured output. Process requests receive a copied
executable, argument vector and working directory. No worker callback enters Guile or retains an
`SCM` value. `AsyncRuntime::drain()` transfers a typed native result to the Guile bridge, which then
invokes the protected Scheme callback on the editor thread. The task record is removed before the
callback runs, allowing callbacks to start or cancel other tasks without re-entering a live record.

An interaction provider may return a typed request with a Scheme result transform. The bridge
tracks that native callback in the same task set as `start-async-task!`; after draining, it converts
the native result to the stable Scheme value form, invokes the transform, validates the candidate
vector, and hands it to the interaction generation that requested it. Refresh and shutdown use the
same cancellation path.

Destroying the adapter makes its callbacks inert and requests cancellation of every native task.
The Guile runtime independently releases its protected procedures, so undrained completion records
cannot access an expired interpreter.
