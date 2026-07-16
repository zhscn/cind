#include "async/runtime.hpp"

#include <uv.h>

#include <atomic>
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
    };

    enum class ReadyKind : std::uint8_t {
        Completed,
        Cancelled,
        Failed,
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

    struct Command {
        CommandKind kind = CommandKind::Submit;
        std::shared_ptr<Task> task;
    };

    struct Ready {
        ReadyKind kind = ReadyKind::Completed;
        std::shared_ptr<Task> task;
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
            discarded = ready.size();
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
            enqueue({.kind = CommandKind::Submit, .task = task});
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
            enqueue({.kind = CommandKind::Cancel, .task = std::move(task)});
        } catch (...) {
            // The stop request remains sufficient for cooperative
            // cancellation even if allocating the loop command fails.
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
            ready.push_back({.kind = kind, .task = task});
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
                }
            } catch (...) {
                if (!first_exception) {
                    first_exception = std::current_exception();
                }
            }
            outstanding.fetch_sub(1, std::memory_order_relaxed);
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

std::size_t AsyncRuntime::drain() {
    return impl_->drain_ready();
}

bool AsyncRuntime::has_work() const {
    return impl_->outstanding.load(std::memory_order_relaxed) != 0;
}

} // namespace cind
