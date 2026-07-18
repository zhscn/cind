#include "async/runtime.hpp"

#include <uv.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cind {

namespace {

class UvErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "libuv"; }
    std::string message(int status) const override { return uv_strerror(status); }
};

const std::error_category& uv_error_category() {
    static const UvErrorCategory category;
    return category;
}

std::system_error uv_error(int status, std::string_view message) {
    return {std::error_code(status, uv_error_category()), std::string(message)};
}

} // namespace

struct AsyncRuntime::Impl {
    enum class CommandKind : std::uint8_t {
        Submit,
        Cancel,
        StartWatch,
        StopWatch,
        StartProcess,
        TerminateProcess,
    };

    enum class ReadyKind : std::uint8_t {
        Completed,
        Cancelled,
        Failed,
        WatchStarted,
        WatchChanged,
        WatchFailed,
        ProcessCompleted,
        ProcessCancelled,
        ProcessFailed,
    };

    struct Task {
        AsyncTaskId id;
        uv_work_t request{};
        std::stop_source cancellation;
        AsyncTaskSpec spec;
        AsyncCompletion completion;
        std::exception_ptr exception;
        bool cancellation_acknowledged = false;
        bool queued = false;
        bool finished = false;
    };

    struct Watch;
    struct Process;

    struct Command {
        CommandKind kind = CommandKind::Submit;
        std::shared_ptr<Task> task;
        std::shared_ptr<Watch> watch;
        std::shared_ptr<Process> process;
    };

    struct Watch {
        Impl* owner = nullptr;
        AsyncWatchId id;
        uv_fs_event_t handle{};
        AsyncDirectoryWatchSpec spec;
        std::atomic_bool stopped = false;
        bool initialized = false;
        bool active = false;
        bool closing = false;
    };

    struct ProcessPipe {
        Process* process = nullptr;
        uv_pipe_t handle{};
        bool standard_error = false;
        bool initialized = false;
        bool closing = false;
    };

    struct Process {
        Impl* owner = nullptr;
        AsyncProcessId id;
        uv_process_t handle{};
        AsyncProcessSpec spec;
        ProcessPipe standard_output;
        ProcessPipe standard_error;
        std::vector<std::string> arguments;
        std::vector<char*> argument_pointers;
        std::string output;
        std::string error_output;
        std::exception_ptr exception;
        std::atomic_bool stopped = false;
        std::int64_t exit_status = 0;
        int term_signal = 0;
        bool started = false;
        bool exited = false;
        bool handle_closed = false;
        bool finished = false;
        bool suppress_delivery = false;
    };

    struct Ready {
        ReadyKind kind = ReadyKind::Completed;
        std::shared_ptr<Task> task;
        std::shared_ptr<Watch> watch;
        AsyncWatchEvent event;
        std::shared_ptr<Process> process;
        AsyncProcessResult process_result;
        std::exception_ptr exception;
    };

    explicit Impl(Wakeup requested_wakeup) : wakeup(std::move(requested_wakeup)) {
        const int loop_status = uv_loop_init(&loop);
        if (loop_status < 0) {
            throw uv_error(loop_status, "cannot initialize libuv loop");
        }
        submit_async.data = this;
        const int async_status = uv_async_init(&loop, &submit_async, on_async);
        if (async_status < 0) {
            (void)uv_loop_close(&loop);
            throw uv_error(async_status, "cannot initialize libuv async handle");
        }
        try {
            loop_thread = std::thread([this] { run_loop(); });
        } catch (...) {
            uv_close(reinterpret_cast<uv_handle_t*>(&submit_async), nullptr);
            (void)uv_run(&loop, UV_RUN_DEFAULT);
            (void)uv_loop_close(&loop);
            throw;
        }
    }

    ~Impl() {
        shutting_down.store(true, std::memory_order_release);
        if (uv_async_send(&submit_async) < 0) {
            std::terminate();
        }
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
        std::size_t discarded = 0;
        {
            std::scoped_lock lock(ready_mutex);
            discarded =
                static_cast<std::size_t>(std::ranges::count_if(ready, [](const Ready& item) {
                    return item.task != nullptr || item.process != nullptr;
                }));
            ready.clear();
        }
        outstanding.fetch_sub(discarded, std::memory_order_relaxed);
    }

    AsyncTaskId submit(AsyncTaskSpec spec) {
        if (!spec.work) {
            throw std::invalid_argument("async task has no work callback");
        }
        if (shutting_down.load(std::memory_order_acquire)) {
            throw std::runtime_error("async runtime is shutting down");
        }
        auto task = std::make_shared<Task>();
        task->id = AsyncTaskId{next_id.fetch_add(1, std::memory_order_relaxed)};
        const AsyncTaskId id = task->id;
        task->spec = std::move(spec);
        task->request.data = task.get();
        {
            std::scoped_lock lock(tasks_mutex);
            if (shutting_down.load(std::memory_order_acquire)) {
                throw std::runtime_error("async runtime is shutting down");
            }
            tasks.emplace(task->id.value, task);
        }
        outstanding.fetch_add(1, std::memory_order_relaxed);
        try {
            enqueue({.kind = CommandKind::Submit, .task = task, .watch = {}, .process = {}});
        } catch (...) {
            {
                std::scoped_lock lock(tasks_mutex);
                tasks.erase(task->id.value);
            }
            outstanding.fetch_sub(1, std::memory_order_relaxed);
            throw;
        }
        return id;
    }

    bool cancel(AsyncTaskId id) noexcept {
        if (!id.valid()) {
            return false;
        }
        std::shared_ptr<Task> task;
        {
            std::scoped_lock lock(tasks_mutex);
            const auto found = tasks.find(id.value);
            if (found == tasks.end()) {
                return false;
            }
            task = found->second;
        }
        (void)task->cancellation.request_stop();
        try {
            enqueue(
                {.kind = CommandKind::Cancel, .task = std::move(task), .watch = {}, .process = {}});
        } catch (...) {
            // The stop request remains sufficient for cooperative
            // cancellation even if allocating the loop command fails.
            return true;
        }
        return true;
    }

    AsyncWatchId watch_directory(AsyncDirectoryWatchSpec spec) {
        if (spec.path.empty()) {
            throw std::invalid_argument("directory watch has no path");
        }
        if (!spec.changed) {
            throw std::invalid_argument("directory watch has no change callback");
        }
        if (shutting_down.load(std::memory_order_acquire)) {
            throw std::runtime_error("async runtime is shutting down");
        }
        auto watch = std::make_shared<Watch>();
        watch->owner = this;
        watch->id = AsyncWatchId{next_id.fetch_add(1, std::memory_order_relaxed)};
        const AsyncWatchId id = watch->id;
        watch->spec = std::move(spec);
        {
            std::scoped_lock lock(watches_mutex);
            if (shutting_down.load(std::memory_order_acquire)) {
                throw std::runtime_error("async runtime is shutting down");
            }
            watches.emplace(id.value, watch);
        }
        try {
            enqueue({.kind = CommandKind::StartWatch, .task = {}, .watch = watch, .process = {}});
        } catch (...) {
            std::scoped_lock lock(watches_mutex);
            watches.erase(id.value);
            throw;
        }
        return id;
    }

    bool unwatch(AsyncWatchId id) noexcept {
        if (!id.valid()) {
            return false;
        }
        std::shared_ptr<Watch> watch;
        {
            std::scoped_lock lock(watches_mutex);
            const auto found = watches.find(id.value);
            if (found == watches.end()) {
                return false;
            }
            watch = found->second;
        }
        if (watch->stopped.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        try {
            enqueue({.kind = CommandKind::StopWatch,
                     .task = {},
                     .watch = std::move(watch),
                     .process = {}});
        } catch (...) {
            return true;
        }
        return true;
    }

    AsyncProcessId spawn(AsyncProcessSpec spec) {
        if (spec.file.empty()) {
            throw std::invalid_argument("async process has no executable");
        }
        if (!spec.completed) {
            throw std::invalid_argument("async process has no completion callback");
        }
        if (shutting_down.load(std::memory_order_acquire)) {
            throw std::runtime_error("async runtime is shutting down");
        }
        auto process = std::make_shared<Process>();
        process->owner = this;
        process->id = AsyncProcessId{next_id.fetch_add(1, std::memory_order_relaxed)};
        const AsyncProcessId id = process->id;
        process->spec = std::move(spec);
        process->standard_output.process = process.get();
        process->standard_error.process = process.get();
        process->standard_error.standard_error = true;
        {
            std::scoped_lock lock(processes_mutex);
            if (shutting_down.load(std::memory_order_acquire)) {
                throw std::runtime_error("async runtime is shutting down");
            }
            processes.emplace(id.value, process);
        }
        outstanding.fetch_add(1, std::memory_order_relaxed);
        try {
            enqueue(
                {.kind = CommandKind::StartProcess, .task = {}, .watch = {}, .process = process});
        } catch (...) {
            {
                std::scoped_lock lock(processes_mutex);
                processes.erase(id.value);
            }
            outstanding.fetch_sub(1, std::memory_order_relaxed);
            throw;
        }
        return id;
    }

    bool terminate(AsyncProcessId id) noexcept {
        if (!id.valid()) {
            return false;
        }
        std::shared_ptr<Process> process;
        {
            std::scoped_lock lock(processes_mutex);
            const auto found = processes.find(id.value);
            if (found == processes.end()) {
                return false;
            }
            process = found->second;
        }
        if (process->stopped.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        try {
            enqueue({.kind = CommandKind::TerminateProcess,
                     .task = {},
                     .watch = {},
                     .process = std::move(process)});
        } catch (...) {
            return true;
        }
        return true;
    }

    void enqueue(Command command) {
        {
            std::scoped_lock lock(command_mutex);
            commands.push_back(std::move(command));
        }
        const int status = uv_async_send(&submit_async);
        if (status < 0) {
            std::terminate();
        }
    }

    static void on_async(uv_async_t* handle) { static_cast<Impl*>(handle->data)->drain_commands(); }

    void drain_commands() {
        while (true) {
            Command command;
            {
                std::scoped_lock lock(command_mutex);
                if (commands.empty()) {
                    break;
                }
                command = std::move(commands.front());
                commands.pop_front();
            }
            switch (command.kind) {
            case CommandKind::Submit:
                queue_task(command.task);
                break;
            case CommandKind::Cancel:
                cancel_task(command.task);
                break;
            case CommandKind::StartWatch:
                start_watch(command.watch);
                break;
            case CommandKind::StopWatch:
                stop_watch(command.watch);
                break;
            case CommandKind::StartProcess:
                start_process(command.process);
                break;
            case CommandKind::TerminateProcess:
                terminate_process(command.process, SIGTERM);
                break;
            }
        }
        if (shutting_down.load(std::memory_order_acquire)) {
            begin_shutdown();
        }
    }

    void queue_task(const std::shared_ptr<Task>& task) {
        if (task->finished) {
            return;
        }
        if (shutting_down.load(std::memory_order_acquire)) {
            finish_without_delivery(task);
            return;
        }
        if (task->cancellation.stop_requested()) {
            publish(task, ReadyKind::Cancelled);
            return;
        }
        task->queued = true;
        ++active_requests;
        const int status = uv_queue_work(&loop, &task->request, run_work, after_work);
        if (status < 0) {
            --active_requests;
            task->queued = false;
            task->exception = std::make_exception_ptr(uv_error(status, "cannot queue async work"));
            publish(task, ReadyKind::Failed);
        }
    }

    void cancel_task(const std::shared_ptr<Task>& task) {
        if (task->finished || !task->queued) {
            return;
        }
        (void)uv_cancel(reinterpret_cast<uv_req_t*>(&task->request));
    }

    void start_watch(const std::shared_ptr<Watch>& watch) {
        if (!watch || watch->stopped.load(std::memory_order_acquire) ||
            shutting_down.load(std::memory_order_acquire)) {
            erase_watch(watch);
            return;
        }
        int status = uv_fs_event_init(&loop, &watch->handle);
        if (status < 0) {
            publish_watch_failure(watch, status, "cannot initialize directory watch");
            erase_watch(watch);
            return;
        }
        watch->initialized = true;
        watch->handle.data = watch.get();
        status = uv_fs_event_start(&watch->handle, on_fs_event, watch->spec.path.c_str(), 0);
        if (status < 0) {
            publish_watch_failure(watch, status, "cannot start directory watch");
            close_watch(watch);
            return;
        }
        watch->active = true;
        publish_ready({.kind = ReadyKind::WatchStarted,
                       .task = {},
                       .watch = watch,
                       .event = {},
                       .process = {},
                       .process_result = {},
                       .exception = {}});
    }

    void stop_watch(const std::shared_ptr<Watch>& watch) {
        if (!watch) {
            return;
        }
        if (!watch->initialized) {
            erase_watch(watch);
            return;
        }
        close_watch(watch);
    }

    void start_process(const std::shared_ptr<Process>& process) {
        if (!process || process->finished) {
            return;
        }
        if (shutting_down.load(std::memory_order_acquire)) {
            process->suppress_delivery = true;
            process->exited = true;
            process->handle_closed = true;
            maybe_finish_process(process);
            return;
        }
        if (process->stopped.load(std::memory_order_acquire)) {
            process->exited = true;
            process->handle_closed = true;
            maybe_finish_process(process);
            return;
        }
        try {
            process->arguments.reserve(process->spec.arguments.size() + 1);
            process->arguments.push_back(process->spec.file);
            process->arguments.insert(process->arguments.end(), process->spec.arguments.begin(),
                                      process->spec.arguments.end());
            process->argument_pointers.reserve(process->arguments.size() + 1);
            for (std::string& argument : process->arguments) {
                process->argument_pointers.push_back(argument.data());
            }
            process->argument_pointers.push_back(nullptr);

            int status = initialize_process_pipe(process->standard_output);
            if (status >= 0) {
                status = initialize_process_pipe(process->standard_error);
            }
            if (status < 0) {
                process->exception =
                    std::make_exception_ptr(uv_error(status, "cannot initialize process pipes"));
                process->exited = true;
                process->handle_closed = true;
                close_process_pipe(process->standard_output);
                close_process_pipe(process->standard_error);
                maybe_finish_process(process);
                return;
            }

            uv_stdio_container_t stdio[3]{};
            stdio[0].flags = UV_IGNORE;
            // libuv declares stdio flags as an enum but specifies these values as a bitmask.
            // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
            stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&process->standard_output.handle);
            // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
            stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&process->standard_error.handle);

            uv_process_options_t options{};
            options.exit_cb = on_process_exit;
            options.file = process->spec.file.c_str();
            options.args = process->argument_pointers.data();
            options.cwd = process->spec.working_directory.empty()
                              ? nullptr
                              : process->spec.working_directory.c_str();
            options.stdio_count = 3;
            options.stdio = stdio;
            process->handle.data = process.get();
            status = uv_spawn(&loop, &process->handle, &options);
            if (status < 0) {
                process->exception =
                    std::make_exception_ptr(uv_error(status, "cannot start process"));
                process->exited = true;
                uv_handle_t* process_handle = reinterpret_cast<uv_handle_t*>(&process->handle);
                if (uv_handle_get_type(process_handle) == UV_PROCESS) {
                    uv_close(process_handle, on_process_closed);
                } else {
                    process->handle_closed = true;
                }
                close_process_pipe(process->standard_output);
                close_process_pipe(process->standard_error);
                maybe_finish_process(process);
                return;
            }
            process->started = true;
            status = start_process_read(process->standard_output);
            if (status >= 0) {
                status = start_process_read(process->standard_error);
            }
            if (status < 0) {
                process->exception =
                    std::make_exception_ptr(uv_error(status, "cannot read process output"));
                close_process_pipe(process->standard_output);
                close_process_pipe(process->standard_error);
                (void)uv_process_kill(&process->handle, SIGTERM);
            }
        } catch (...) {
            process->exception = std::current_exception();
            if (process->started) {
                (void)uv_process_kill(&process->handle, SIGTERM);
            } else {
                process->exited = true;
                process->handle_closed = true;
                close_process_pipe(process->standard_output);
                close_process_pipe(process->standard_error);
                maybe_finish_process(process);
            }
        }
    }

    int initialize_process_pipe(ProcessPipe& pipe) {
        const int status = uv_pipe_init(&loop, &pipe.handle, 0);
        if (status >= 0) {
            pipe.initialized = true;
            pipe.handle.data = &pipe;
        }
        return status;
    }

    static void allocate_process_read(uv_handle_t*, std::size_t suggested, uv_buf_t* buffer) {
        constexpr std::size_t maximum_chunk = std::size_t{64} * 1024;
        const std::size_t size = std::min(suggested, maximum_chunk);
        buffer->base = new (std::nothrow) char[size];
        buffer->len = buffer->base != nullptr ? size : 0;
    }

    int start_process_read(ProcessPipe& pipe) {
        const int status = uv_read_start(reinterpret_cast<uv_stream_t*>(&pipe.handle),
                                         allocate_process_read, on_process_read);
        if (status < 0) {
            close_process_pipe(pipe);
        }
        return status;
    }

    static void on_process_read(uv_stream_t* stream, std::ptrdiff_t count, const uv_buf_t* buffer) {
        auto* pipe = static_cast<ProcessPipe*>(stream->data);
        Process& process = *pipe->process;
        try {
            if (count > 0) {
                constexpr std::size_t maximum_output = std::size_t{64} * 1024 * 1024;
                std::string& output = pipe->standard_error ? process.error_output : process.output;
                const std::size_t size = static_cast<std::size_t>(count);
                if (size > maximum_output - std::min(output.size(), maximum_output)) {
                    throw std::length_error("process output exceeds 64 MiB");
                }
                output.append(buffer->base, size);
            } else if (count < 0) {
                if (count != UV_EOF && !process.exception) {
                    process.exception = std::make_exception_ptr(
                        uv_error(static_cast<int>(count), "cannot read process output"));
                    if (process.started && !process.exited) {
                        (void)uv_process_kill(&process.handle, SIGTERM);
                    }
                }
                process.owner->close_process_pipe(*pipe);
            }
        } catch (...) {
            process.exception = std::current_exception();
            process.owner->close_process_pipe(*pipe);
            if (process.started && !process.exited) {
                (void)uv_process_kill(&process.handle, SIGTERM);
            }
        }
        delete[] buffer->base;
    }

    void close_process_pipe(ProcessPipe& pipe) {
        if (!pipe.initialized || pipe.closing) {
            return;
        }
        pipe.closing = true;
        (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&pipe.handle));
        uv_close(reinterpret_cast<uv_handle_t*>(&pipe.handle), on_process_pipe_closed);
    }

    static void on_process_pipe_closed(uv_handle_t* handle) {
        auto* pipe = static_cast<ProcessPipe*>(handle->data);
        Process& process = *pipe->process;
        pipe->initialized = false;
        process.owner->maybe_finish_process(process.owner->find_process(process.id));
    }

    // Signature is fixed by uv_exit_cb.
    static void on_process_exit(uv_process_t* handle, std::int64_t exit_status, int term_signal) {
        auto* raw_process = static_cast<Process*>(handle->data);
        std::shared_ptr<Process> process = raw_process->owner->find_process(raw_process->id);
        if (!process) {
            return;
        }
        process->exited = true;
        process->exit_status = exit_status;
        process->term_signal = term_signal;
        uv_close(reinterpret_cast<uv_handle_t*>(&process->handle), on_process_closed);
    }

    static void on_process_closed(uv_handle_t* handle) {
        auto* raw_process = static_cast<Process*>(handle->data);
        std::shared_ptr<Process> process = raw_process->owner->find_process(raw_process->id);
        if (!process) {
            return;
        }
        process->handle_closed = true;
        process->owner->maybe_finish_process(process);
    }

    void terminate_process(const std::shared_ptr<Process>& process, int signal) {
        if (!process || process->finished) {
            return;
        }
        if (!process->started) {
            process->exited = true;
            process->handle_closed = true;
            maybe_finish_process(process);
            return;
        }
        if (!process->exited) {
            const int status = uv_process_kill(&process->handle, signal);
            if (status < 0 && status != UV_ESRCH && !process->exception) {
                process->exception =
                    std::make_exception_ptr(uv_error(status, "cannot terminate process"));
            }
        }
    }

    std::shared_ptr<Process> find_process(AsyncProcessId id) {
        std::scoped_lock lock(processes_mutex);
        const auto found = processes.find(id.value);
        return found != processes.end() ? found->second : std::shared_ptr<Process>{};
    }

    void maybe_finish_process(const std::shared_ptr<Process>& process) {
        if (!process || process->finished || !process->exited || !process->handle_closed ||
            process->standard_output.initialized || process->standard_error.initialized) {
            return;
        }
        process->finished = true;
        {
            std::scoped_lock lock(processes_mutex);
            processes.erase(process->id.value);
        }
        if (process->suppress_delivery || shutting_down.load(std::memory_order_acquire)) {
            outstanding.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        ReadyKind kind = ReadyKind::ProcessCompleted;
        if (process->exception) {
            kind = ReadyKind::ProcessFailed;
        } else if (process->stopped.load(std::memory_order_acquire)) {
            kind = ReadyKind::ProcessCancelled;
        }
        Ready ready_item{.kind = kind,
                         .task = {},
                         .watch = {},
                         .event = {},
                         .process = process,
                         .process_result = {.process = process->id,
                                            .exit_status = process->exit_status,
                                            .term_signal = process->term_signal,
                                            .standard_output = std::move(process->output),
                                            .standard_error = std::move(process->error_output)},
                         .exception = process->exception};
        publish_ready(std::move(ready_item));
    }

    void close_watch(const std::shared_ptr<Watch>& watch) {
        if (watch->closing) {
            return;
        }
        watch->closing = true;
        if (watch->active) {
            (void)uv_fs_event_stop(&watch->handle);
            watch->active = false;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(&watch->handle), on_watch_closed);
    }

    static void on_watch_closed(uv_handle_t* handle) {
        auto* watch = static_cast<Watch*>(handle->data);
        watch->owner->erase_watch(watch->id);
    }

    // Signature is fixed by uv_fs_event_cb.
    static void on_fs_event(uv_fs_event_t* handle, const char* filename, int events, int status) {
        auto* raw_watch = static_cast<Watch*>(handle->data);
        Impl& self = *raw_watch->owner;
        std::shared_ptr<Watch> watch = self.find_watch(raw_watch->id);
        if (!watch || watch->stopped.load(std::memory_order_acquire)) {
            return;
        }
        if (status < 0) {
            self.publish_watch_failure(watch, status, "directory watch failed");
            self.close_watch(watch);
            return;
        }
        std::string path = watch->spec.path;
        if (filename != nullptr && *filename != '\0') {
            if (!path.empty() && path.back() != '/') {
                path.push_back('/');
            }
            path.append(filename);
        }
        self.publish_ready({.kind = ReadyKind::WatchChanged,
                            .task = {},
                            .watch = watch,
                            .event = {.watch = watch->id,
                                      .directory = watch->spec.path,
                                      .path = std::move(path),
                                      .renamed = (events & UV_RENAME) != 0,
                                      .changed = (events & UV_CHANGE) != 0},
                            .process = {},
                            .process_result = {},
                            .exception = {}});
    }

    std::shared_ptr<Watch> find_watch(AsyncWatchId id) {
        std::scoped_lock lock(watches_mutex);
        const auto found = watches.find(id.value);
        return found != watches.end() ? found->second : std::shared_ptr<Watch>{};
    }

    void erase_watch(const std::shared_ptr<Watch>& watch) {
        if (watch) {
            erase_watch(watch->id);
        }
    }

    void erase_watch(AsyncWatchId id) {
        std::scoped_lock lock(watches_mutex);
        watches.erase(id.value);
    }

    void publish_watch_failure(const std::shared_ptr<Watch>& watch, int status,
                               std::string_view message) {
        Ready item{.kind = ReadyKind::WatchFailed,
                   .task = {},
                   .watch = watch,
                   .event = {},
                   .process = {},
                   .process_result = {},
                   .exception = {}};
        item.exception = std::make_exception_ptr(uv_error(status, message));
        publish_ready(std::move(item));
    }

    void publish_ready(Ready item) {
        {
            std::scoped_lock lock(ready_mutex);
            ready.push_back(std::move(item));
        }
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
                return;
            }
        }
    }

    static void run_work(uv_work_t* request) {
        Task& task = *static_cast<Task*>(request->data);
        try {
            task.completion = task.spec.work(task.cancellation.get_token());
        } catch (const AsyncTaskCancelled&) {
            task.cancellation_acknowledged = true;
        } catch (...) {
            task.exception = std::current_exception();
        }
    }

    static void after_work(uv_work_t* request, int status) {
        Task& task = *static_cast<Task*>(request->data);
        Impl& self = *static_cast<Impl*>(request->loop->data);
        task.queued = false;
        --self.active_requests;
        if (self.shutting_down.load(std::memory_order_acquire)) {
            self.finish_without_delivery(self.find_task(task.id));
        } else if (status == UV_ECANCELED || task.cancellation_acknowledged) {
            self.publish(self.find_task(task.id), ReadyKind::Cancelled);
        } else if (task.exception) {
            self.publish(self.find_task(task.id), ReadyKind::Failed);
        } else {
            self.publish(self.find_task(task.id), ReadyKind::Completed);
        }
        self.close_async_if_idle();
    }

    std::shared_ptr<Task> find_task(AsyncTaskId id) {
        std::scoped_lock lock(tasks_mutex);
        const auto found = tasks.find(id.value);
        return found != tasks.end() ? found->second : std::shared_ptr<Task>{};
    }

    void publish(const std::shared_ptr<Task>& task, ReadyKind kind) {
        if (!task || task->finished) {
            return;
        }
        task->finished = true;
        {
            std::scoped_lock lock(tasks_mutex);
            tasks.erase(task->id.value);
        }
        {
            std::scoped_lock lock(ready_mutex);
            ready.push_back({.kind = kind,
                             .task = task,
                             .watch = {},
                             .event = {},
                             .process = {},
                             .process_result = {},
                             .exception = {}});
        }
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
                return;
            }
        }
    }

    void finish_without_delivery(const std::shared_ptr<Task>& task) {
        if (!task || task->finished) {
            return;
        }
        task->finished = true;
        {
            std::scoped_lock lock(tasks_mutex);
            tasks.erase(task->id.value);
        }
        outstanding.fetch_sub(1, std::memory_order_relaxed);
    }

    void begin_shutdown() {
        std::vector<std::shared_ptr<Task>> pending;
        {
            std::scoped_lock lock(tasks_mutex);
            pending.reserve(tasks.size());
            for (const auto& [id, task] : tasks) {
                (void)id;
                pending.push_back(task);
            }
        }
        for (const std::shared_ptr<Task>& task : pending) {
            (void)task->cancellation.request_stop();
            if (task->queued) {
                (void)uv_cancel(reinterpret_cast<uv_req_t*>(&task->request));
            } else {
                finish_without_delivery(task);
            }
        }
        std::vector<std::shared_ptr<Watch>> active_watches;
        {
            std::scoped_lock lock(watches_mutex);
            active_watches.reserve(watches.size());
            for (const auto& [id, watch] : watches) {
                (void)id;
                active_watches.push_back(watch);
            }
        }
        for (const std::shared_ptr<Watch>& watch : active_watches) {
            watch->stopped.store(true, std::memory_order_release);
            stop_watch(watch);
        }
        std::vector<std::shared_ptr<Process>> active_processes;
        {
            std::scoped_lock lock(processes_mutex);
            active_processes.reserve(processes.size());
            for (const auto& [id, process] : processes) {
                (void)id;
                active_processes.push_back(process);
            }
        }
        for (const std::shared_ptr<Process>& process : active_processes) {
            process->stopped.store(true, std::memory_order_release);
            process->suppress_delivery = true;
            terminate_process(process, SIGKILL);
        }
        close_async_if_idle();
    }

    void close_async_if_idle() {
        if (shutting_down.load(std::memory_order_acquire) && active_requests == 0 &&
            !uv_is_closing(reinterpret_cast<uv_handle_t*>(&submit_async))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&submit_async), nullptr);
        }
    }

    std::size_t drain_ready() {
        std::vector<Ready> pending;
        {
            std::scoped_lock lock(ready_mutex);
            pending.reserve(ready.size());
            while (!ready.empty()) {
                pending.push_back(std::move(ready.front()));
                ready.pop_front();
            }
        }
        std::exception_ptr first_exception;
        for (Ready& item : pending) {
            try {
                switch (item.kind) {
                case ReadyKind::Completed:
                    if (item.task->completion) {
                        item.task->completion();
                    }
                    break;
                case ReadyKind::Cancelled:
                    if (item.task->spec.cancelled) {
                        item.task->spec.cancelled();
                    }
                    break;
                case ReadyKind::Failed:
                    if (item.task->spec.failed) {
                        item.task->spec.failed(item.task->exception);
                    }
                    break;
                case ReadyKind::WatchStarted:
                    if (!item.watch->stopped.load(std::memory_order_acquire) &&
                        item.watch->spec.started) {
                        item.watch->spec.started();
                    }
                    break;
                case ReadyKind::WatchChanged:
                    if (!item.watch->stopped.load(std::memory_order_acquire) &&
                        item.watch->spec.changed) {
                        item.watch->spec.changed(item.event);
                    }
                    break;
                case ReadyKind::WatchFailed:
                    if (!item.watch->stopped.load(std::memory_order_acquire) &&
                        item.watch->spec.failed) {
                        item.watch->spec.failed(item.exception);
                    }
                    break;
                case ReadyKind::ProcessCompleted:
                    if (item.process->spec.completed) {
                        item.process->spec.completed(std::move(item.process_result));
                    }
                    break;
                case ReadyKind::ProcessCancelled:
                    if (item.process->spec.cancelled) {
                        item.process->spec.cancelled();
                    }
                    break;
                case ReadyKind::ProcessFailed:
                    if (item.process->spec.failed) {
                        item.process->spec.failed(item.exception);
                    }
                    break;
                }
            } catch (...) {
                if (!first_exception) {
                    first_exception = std::current_exception();
                }
            }
            if (item.task || item.process) {
                outstanding.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        if (first_exception) {
            std::rethrow_exception(first_exception);
        }
        return pending.size();
    }

    void run_loop() {
        loop.data = this;
        (void)uv_run(&loop, UV_RUN_DEFAULT);
        (void)uv_loop_close(&loop);
    }

    Wakeup wakeup;
    uv_loop_t loop{};
    uv_async_t submit_async{};
    std::thread loop_thread;
    std::atomic_bool shutting_down = false;
    std::atomic_uint64_t next_id = 1;
    std::atomic_size_t outstanding = 0;
    std::size_t active_requests = 0;

    std::mutex command_mutex;
    std::deque<Command> commands;
    std::mutex tasks_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Task>> tasks;
    std::mutex watches_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Watch>> watches;
    std::mutex processes_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Process>> processes;
    std::mutex ready_mutex;
    std::deque<Ready> ready;
};

AsyncRuntime::AsyncRuntime(Wakeup wakeup) : impl_(std::make_unique<Impl>(std::move(wakeup))) {}

AsyncRuntime::~AsyncRuntime() = default;

AsyncTaskId AsyncRuntime::submit(AsyncTaskSpec spec) {
    return impl_->submit(std::move(spec));
}

bool AsyncRuntime::cancel(AsyncTaskId task) noexcept {
    return impl_->cancel(task);
}

AsyncWatchId AsyncRuntime::watch_directory(AsyncDirectoryWatchSpec spec) {
    return impl_->watch_directory(std::move(spec));
}

bool AsyncRuntime::unwatch(AsyncWatchId watch) noexcept {
    return impl_->unwatch(watch);
}

AsyncProcessId AsyncRuntime::spawn(AsyncProcessSpec spec) {
    return impl_->spawn(std::move(spec));
}

bool AsyncRuntime::terminate(AsyncProcessId process) noexcept {
    return impl_->terminate(process);
}

std::size_t AsyncRuntime::drain() {
    return impl_->drain_ready();
}

bool AsyncRuntime::has_work() const {
    return impl_->outstanding.load(std::memory_order_relaxed) != 0;
}

} // namespace cind
