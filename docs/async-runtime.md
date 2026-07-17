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
the runtime after the state captured by its callbacks so this shutdown completes before buffers
and registries are destroyed.

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

File reads, directory enumeration and atomic saves run as worker tasks. Application startup opens
its requested file asynchronously and replaces the temporary scratch buffer after the read, style
discovery and project discovery complete. Project discovery consumes an immutable snapshot of the
Scheme-defined provider registry, so worker tasks never call Guile or read mutable editor state.
File save captures an immutable document snapshot and
performs the atomic file replacement as one worker task. Its completion marks that snapshot as the
save point. Edits made while the write is in progress stay modified, so asynchronous completion
cannot mark newer content as saved.

Project indexing uses worker tasks, directory watches and generation checks. Project search runs
`rg` through the process service, parses its completed output on the worker pool and creates a
read-only location-list buffer on the editor thread.

Modes and services can submit additional work through `EditorApplication::async_runtime()`. Their
completion callbacks must validate any resource identity or revision they captured before applying
results. Long-running work should check its cancellation token between independently safe units of
work.
