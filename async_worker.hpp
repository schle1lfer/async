#pragma once

/**
 * async_worker.hpp  –  C++23 / Linux
 *
 * A thread-pool-based async task dispatcher.
 *
 * Design overview
 * ───────────────
 *  • Tasks are named callable units registered by the caller.  The callable
 *    must conform to TaskFn (input/output are described via iovec arrays).
 *  • A task is submitted via submit(), which returns a unique uint64_t ID.
 *  • The worker pool (size fixed at construction) executes tasks from an
 *    internal FIFO queue.  Each task transitions: PENDING → RUNNING → COMPLETED.
 *  • Input data is deep-copied into owned buffers at submit time (the caller's
 *    memory need not remain alive).  Output buffers are pre-allocated by the
 *    worker; the caller retrieves them via get() after the task completes.
 *  • All per-task state is held in a TaskDescriptor, accessible by ID.
 *
 * Parameter passing via iovec
 * ───────────────────────────
 *  iovec is the standard Linux scatter/gather descriptor (sys/uio.h):
 *      struct iovec { void *iov_base; size_t iov_len; };
 *
 *  For INPUT  params: iov_base + iov_len describe caller-owned data.
 *                     AsyncWorker deep-copies every buffer at submit time.
 *  For OUTPUT params: only iov_len is examined at submit time;
 *                     AsyncWorker allocates an internal buffer of that size.
 *                     The TaskFn writes its results there; the caller reads
 *                     them from TaskDescriptor::out_params after completion.
 *
 * Thread safety
 * ─────────────
 *  All public methods are safe to call concurrently from any thread.
 */

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/uio.h>   // iovec — POSIX / Linux

namespace async {

// ─────────────────────────────────────────────────────────────────────────────
// TaskFn  –  canonical signature for every executable function
// ─────────────────────────────────────────────────────────────────────────────
//
// Parameters
//   in       : array of in_count iovec descriptors pointing to input data
//              (the memory is owned by the TaskDescriptor; treat as read-only)
//   in_count : number of input descriptors
//   out      : array of out_count iovec descriptors pointing to output buffers
//              (pre-allocated by AsyncWorker; write results here)
//   out_count: number of output descriptors
//
// Returns   : caller-defined result code stored in TaskDescriptor::return_code
//
using TaskFn = std::function<int(const iovec* in,  std::size_t in_count,
                                  iovec*       out, std::size_t out_count)>;


// ─────────────────────────────────────────────────────────────────────────────
// TaskStatus
// ─────────────────────────────────────────────────────────────────────────────
enum class TaskStatus : std::uint8_t {
    PENDING   = 0,  ///< Queued, not yet started
    RUNNING   = 1,  ///< Being executed by a worker thread
    COMPLETED = 2,  ///< Finished; return_code and out_params are now valid
};


// ─────────────────────────────────────────────────────────────────────────────
// TaskDescriptor  –  all data associated with one execution request
// ─────────────────────────────────────────────────────────────────────────────
struct TaskDescriptor {
    // ── Identity ─────────────────────────────────────────────────────────────
    std::uint64_t id            {};   ///< Unique task identifier (assigned by AsyncWorker)
    std::string   function_name {};   ///< Name used to look up the TaskFn in the registry

    // ── Input parameters ─────────────────────────────────────────────────────
    // Deep copies of the caller-supplied buffers; iov_base pointers are stable.
    std::vector<std::vector<std::byte>> in_data   {};
    std::vector<iovec>                  in_params {};  ///< iov_base → in_data[i].data()
    std::size_t                         in_count  {};  ///< number of input descriptors

    // ── Output parameters ────────────────────────────────────────────────────
    // Buffers allocated at submit time (sized from TaskSubmission::out_params[i].iov_len).
    // The TaskFn writes results here; read after status == COMPLETED.
    std::vector<std::vector<std::byte>> out_data   {};
    std::vector<iovec>                  out_params {};  ///< iov_base → out_data[i].data()
    std::size_t                         out_count  {};  ///< number of output descriptors

    // ── Execution state ───────────────────────────────────────────────────────
    std::atomic<TaskStatus> status      {TaskStatus::PENDING};
    int                     return_code {};  ///< Value returned by TaskFn; valid when COMPLETED

    // Non-copyable (atomic member); constructible/movable only at creation.
    TaskDescriptor()                               = default;
    TaskDescriptor(const TaskDescriptor&)          = delete;
    TaskDescriptor& operator=(const TaskDescriptor&) = delete;
};


// ─────────────────────────────────────────────────────────────────────────────
// TaskSubmission  –  caller fills this and passes to AsyncWorker::submit()
// ─────────────────────────────────────────────────────────────────────────────
struct TaskSubmission {
    std::string  function_name {};

    /// Input data: both iov_base and iov_len are read; data is deep-copied.
    const iovec* in_params  {};
    std::size_t  in_count   {};

    /// Output sizing: only iov_len is read; AsyncWorker allocates the buffers.
    /// Set iov_base to nullptr; it will be replaced with an internal pointer.
    const iovec* out_params {};
    std::size_t  out_count  {};
};


// ─────────────────────────────────────────────────────────────────────────────
// AsyncWorker
// ─────────────────────────────────────────────────────────────────────────────
class AsyncWorker {
public:
    /// Construct a worker pool with @p concurrency execution threads.
    /// @throws std::invalid_argument if concurrency == 0.
    explicit AsyncWorker(std::size_t concurrency);

    /// Destructor: requests stop on all threads and joins them.
    /// Any tasks still PENDING at this point remain in that state (not started).
    /// Any task currently RUNNING is allowed to finish.
    ~AsyncWorker();

    // Non-copyable, non-movable.
    AsyncWorker(const AsyncWorker&)            = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;

    // ── Function registry ─────────────────────────────────────────────────────

    /// Register (or replace) a callable under @p name.
    void register_function(std::string name, TaskFn fn);

    /// Remove a function from the registry.
    /// @return true if the function existed and was removed.
    bool unregister_function(const std::string& name);

    // ── Task lifecycle ────────────────────────────────────────────────────────

    /// Enqueue a task for asynchronous execution.
    /// @return unique task ID.
    /// @throws std::runtime_error if function_name is not registered.
    [[nodiscard]] std::uint64_t submit(TaskSubmission sub);

    /// Query the current execution status of a task.
    /// @return nullopt if no task with this ID exists.
    [[nodiscard]] std::optional<TaskStatus> query(std::uint64_t id) const;

    /// Retrieve the full task descriptor by ID.
    /// The returned shared_ptr keeps the descriptor alive even after purge().
    /// @return nullptr if no task with this ID exists.
    [[nodiscard]] std::shared_ptr<const TaskDescriptor> get(std::uint64_t id) const;

    /// Remove a task from internal storage (frees its memory).
    /// @return true if the task existed and was removed.
    bool purge(std::uint64_t id);

    /// Return the number of tasks currently tracked (any status).
    [[nodiscard]] std::size_t task_count() const;

private:
    void worker_loop(std::stop_token st);

    // ── Function registry ─────────────────────────────────────────────────────
    mutable std::shared_mutex                        registry_mu_;
    std::unordered_map<std::string, TaskFn>          registry_;

    // ── Task store (ID → descriptor) ─────────────────────────────────────────
    mutable std::shared_mutex                                           tasks_mu_;
    std::unordered_map<std::uint64_t,
                       std::shared_ptr<TaskDescriptor>>                 tasks_;

    // ── Work queue ────────────────────────────────────────────────────────────
    std::mutex                  queue_mu_;
    std::condition_variable_any queue_cv_;   // supports stop_token integration
    std::queue<std::uint64_t>   queue_;

    // ── Worker threads ────────────────────────────────────────────────────────
    std::vector<std::jthread>   workers_;

    // ── Monotonically increasing task ID ─────────────────────────────────────
    std::atomic<std::uint64_t>  next_id_{1};
};

} // namespace async
