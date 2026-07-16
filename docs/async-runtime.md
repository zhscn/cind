# Asynchronous runtime

cind keeps editor state single-writer while allowing file operations and other blocking work to
run concurrently. `AsyncRuntime` owns a dedicated libuv loop thread and submits blocking work to
libuv's worker pool. Each `EditorApplication` owns one runtime for the lifetime of its buffers,
views, commands and interactions.

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

## Task lifecycle

`submit()` returns an `AsyncTaskId`. A task remains outstanding until its main-thread callback has
been drained. `has_work()` includes queued work, running work and undrained results, which makes it
suitable for lifecycle checks without using it as an event-loop polling mechanism.

`cancel()` requests the task's standard stop source and asks the loop thread to cancel work that has
not started. Work that may already be running checks the token at safe points.
Cancellation and failure callbacks follow the same main-thread delivery path as successful
completion. A true return value means that the task was still known and received a stop request; it
does not claim that worker execution was preempted.

Destroying the runtime requests cancellation, waits for active libuv requests, closes its async
handle and joins the loop thread. Ready callbacks that have not been drained are discarded during
shutdown. `EditorApplication` declares the runtime after the state captured by its callbacks so
this shutdown completes before buffers and registries are destroyed.

## Editor integration

File save captures an immutable document snapshot and performs the atomic file replacement as one
worker task. Its completion marks that snapshot as the save point. Edits made while the write is in
progress stay modified, so asynchronous completion cannot mark newer content as saved.

Modes and services can submit additional work through `EditorApplication::async_runtime()`. Their
completion callbacks must validate any resource identity or revision they captured before applying
results. Long-running work should check its cancellation token between independently safe units of
work.
