#include "async_worker.hpp"

#include <cstring>
#include <stdexcept>

namespace async {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

AsyncWorker::AsyncWorker(std::size_t concurrency)
{
    if (concurrency == 0)
        throw std::invalid_argument("AsyncWorker: concurrency must be >= 1");

    workers_.reserve(concurrency);
    for (std::size_t i = 0; i < concurrency; ++i) {
        workers_.emplace_back([this](std::stop_token st) {
            worker_loop(std::move(st));
        });
    }
}

AsyncWorker::~AsyncWorker()
{
    // std::jthread::~jthread() calls request_stop() then join() automatically.
    // condition_variable_any::wait(lock, stop_token, pred) registers an internal
    // stop_callback that wakes sleeping threads when stop is requested, so no
    // explicit notify_all() is required here.
}


// ─────────────────────────────────────────────────────────────────────────────
// Function registry
// ─────────────────────────────────────────────────────────────────────────────

void AsyncWorker::register_function(std::string name, TaskFn fn)
{
    std::unique_lock lock(registry_mu_);
    registry_.insert_or_assign(std::move(name), std::move(fn));
}

bool AsyncWorker::unregister_function(const std::string& name)
{
    std::unique_lock lock(registry_mu_);
    return registry_.erase(name) > 0;
}


// ─────────────────────────────────────────────────────────────────────────────
// submit()
// ─────────────────────────────────────────────────────────────────────────────

std::uint64_t AsyncWorker::submit(TaskSubmission sub)
{
    // Validate function is registered before accepting the task.
    {
        std::shared_lock lock(registry_mu_);
        if (!registry_.contains(sub.function_name))
            throw std::runtime_error(
                "AsyncWorker::submit – unknown function: " + sub.function_name);
    }

    auto desc           = std::make_shared<TaskDescriptor>();
    desc->id            = next_id_.fetch_add(1, std::memory_order_relaxed);
    desc->function_name = sub.function_name;

    // ── Deep-copy input data ──────────────────────────────────────────────────
    // Each caller-supplied buffer is copied into a stable, owned vector so that
    // the caller's memory may be modified or freed immediately after submit().
    // The rebuilt iovec descriptors point into those owned copies.
    desc->in_count = sub.in_count;
    desc->in_data.resize(sub.in_count);
    desc->in_params.resize(sub.in_count);

    for (std::size_t i = 0; i < sub.in_count; ++i) {
        const auto* src = static_cast<const std::byte*>(sub.in_params[i].iov_base);
        const std::size_t len = sub.in_params[i].iov_len;

        desc->in_data[i].assign(src, src + len);
        desc->in_params[i].iov_base = desc->in_data[i].data();
        desc->in_params[i].iov_len  = len;
    }

    // ── Allocate output buffers ───────────────────────────────────────────────
    // Only iov_len from the submission is used to determine the buffer size.
    // AsyncWorker owns these buffers; the TaskFn writes into them and the caller
    // reads from them (via get()) after the task reaches COMPLETED status.
    desc->out_count = sub.out_count;
    desc->out_data.resize(sub.out_count);
    desc->out_params.resize(sub.out_count);

    for (std::size_t i = 0; i < sub.out_count; ++i) {
        const std::size_t len = sub.out_params[i].iov_len;

        desc->out_data[i].resize(len, std::byte{0});
        desc->out_params[i].iov_base = desc->out_data[i].data();
        desc->out_params[i].iov_len  = len;
    }

    const std::uint64_t id = desc->id;

    // Store descriptor in the task map.
    {
        std::unique_lock lock(tasks_mu_);
        tasks_.emplace(id, std::move(desc));
    }

    // Push the ID into the work queue and wake one sleeping worker.
    {
        std::lock_guard lock(queue_mu_);
        queue_.push(id);
    }
    queue_cv_.notify_one();

    return id;
}


// ─────────────────────────────────────────────────────────────────────────────
// Query / retrieval
// ─────────────────────────────────────────────────────────────────────────────

std::optional<TaskStatus> AsyncWorker::query(std::uint64_t id) const
{
    std::shared_lock lock(tasks_mu_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end())
        return std::nullopt;
    return it->second->status.load(std::memory_order_acquire);
}

std::shared_ptr<const TaskDescriptor> AsyncWorker::get(std::uint64_t id) const
{
    std::shared_lock lock(tasks_mu_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end())
        return nullptr;
    return it->second;   // shared ownership keeps the descriptor alive
}

bool AsyncWorker::purge(std::uint64_t id)
{
    std::unique_lock lock(tasks_mu_);
    return tasks_.erase(id) > 0;
}

std::size_t AsyncWorker::task_count() const
{
    std::shared_lock lock(tasks_mu_);
    return tasks_.size();
}


// ─────────────────────────────────────────────────────────────────────────────
// Worker loop  (runs on each pool thread)
// ─────────────────────────────────────────────────────────────────────────────

void AsyncWorker::worker_loop(std::stop_token st)
{
    for (;;) {
        std::uint64_t id{};

        // ── Wait for a task or a stop request ────────────────────────────────
        {
            std::unique_lock lock(queue_mu_);

            // condition_variable_any::wait(lock, stop_token, pred):
            //   • Returns true  : pred() became true   → process the task.
            //   • Returns false : stop was requested AND pred() is still false
            //                     → queue is empty and we were asked to stop.
            if (!queue_cv_.wait(lock, st, [this] { return !queue_.empty(); }))
                return;   // graceful exit

            id = queue_.front();
            queue_.pop();
        }

        // ── Retrieve the task descriptor ─────────────────────────────────────
        std::shared_ptr<TaskDescriptor> desc;
        {
            std::shared_lock lock(tasks_mu_);
            const auto it = tasks_.find(id);
            if (it == tasks_.end())
                continue;   // task was purged before we could run it
            desc = it->second;
        }

        // ── PENDING → RUNNING ─────────────────────────────────────────────────
        desc->status.store(TaskStatus::RUNNING, std::memory_order_release);

        // ── Resolve the callable ──────────────────────────────────────────────
        TaskFn fn;
        {
            std::shared_lock lock(registry_mu_);
            const auto it = registry_.find(desc->function_name);
            if (it == registry_.end()) {
                // Function was unregistered between submit and execution.
                desc->return_code = -1;
                desc->status.store(TaskStatus::COMPLETED, std::memory_order_release);
                continue;
            }
            fn = it->second;   // copy the std::function (keeps callable alive)
        }

        // ── Execute ───────────────────────────────────────────────────────────
        desc->return_code = fn(
            desc->in_params.data(),  desc->in_count,
            desc->out_params.data(), desc->out_count
        );

        // ── RUNNING → COMPLETED ───────────────────────────────────────────────
        desc->status.store(TaskStatus::COMPLETED, std::memory_order_release);
    }
}

} // namespace async
