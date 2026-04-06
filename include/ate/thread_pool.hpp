#pragma once
/// @file thread_pool.hpp
/// @brief Fixed-size thread pool built on std::jthread + TaskQueue.
///
/// Design notes
/// ────────────
/// • std::jthread gives us two things for free:
///     1. Automatic join on destruction (no forgotten detach/join).
///     2. A stop_token that we pass into wait_and_pop(), so workers
///        wake up cleanly when we request_stop().
///
/// • submit() packages any callable into a type-erased task, pairs it
///   with a std::promise, and returns the corresponding std::future.
///   This means:
///     – The caller can retrieve the result asynchronously.
///     – Exceptions thrown inside the task propagate to the future.
///
/// • We do NOT hold the queue lock while invoking the task.  The worker
///   pops the task (lock held briefly), then invokes it (no lock).
///   This is critical: if a task re-submits to the pool, holding the
///   lock would deadlock.
///
/// • Thread count defaults to hardware_concurrency().  A value of 0
///   (which hardware_concurrency can return) is clamped to 1.
///
/// Shutdown behaviour
/// ──────────────────
/// 1. request_stop() → every jthread's stop_token fires.
/// 2. TaskQueue::shutdown() → wakes all blocked workers, rejects new pushes.
/// 3. Workers drain any remaining tasks before exiting.
/// 4. ~ThreadPool() destroys the jthreads, which calls join (already stopped).

#include <ate/task_queue.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

namespace ate {

// Forward declaration — full definition in async_result.hpp
// (included at bottom of this file to break the circular dependency).
template <typename T>
class AsyncResult;

// Forward declaration — full definition in coro_task.hpp.
struct PoolScheduler;

class ThreadPool {
public:
    /// Create a pool with @p num_threads workers.
    /// If num_threads == 0, uses hardware_concurrency (clamped to ≥ 1).
    explicit ThreadPool(std::size_t num_threads = 0);

    /// Destructor — requests stop, shuts down queue, and joins all workers.
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Submit a callable for asynchronous execution.
    ///
    /// Returns std::future<R> where R = std::invoke_result_t<F, Args...>.
    ///
    /// If the pool has been shut down, throws std::runtime_error.
    ///
    /// Implementation walk-through:
    ///   1. Wrap the user's callable into a std::packaged_task<R()>.
    ///   2. Extract the future from the packaged_task.
    ///   3. Wrap the packaged_task into a move_only_function<void()>
    ///      (since packaged_task is move-only, std::function won't work).
    ///   4. Push the wrapper onto the task queue.
    template <typename F, typename... Args>
    auto submit(F&& func, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;

        // Step 1: bind arguments and create packaged_task
        auto bound = std::bind_front(std::forward<F>(func),
                                      std::forward<Args>(args)...);
        auto ptask = std::packaged_task<R()>(std::move(bound));

        // Step 2: grab the future before we move the packaged_task away
        auto future = ptask.get_future();

        // Step 3+4: wrap into type-erased void() and enqueue
        bool enqueued = queue_.push(
            [pt = std::move(ptask)]() mutable { pt(); }
        );

        if (!enqueued) {
            throw std::runtime_error("ThreadPool: cannot submit — pool is shut down");
        }

        return future;
    }

    /// Submit and get a chainable AsyncResult<R> instead of a raw future.
    ///
    /// Usage:
    ///   auto result = pool.submit_async([] { return 42; })
    ///                     .then([](int x) { return x * 2; })
    ///                     .then([](int x) { return std::to_string(x); });
    ///   std::string s = result.get();  // "84"
    template <typename F, typename... Args>
    auto submit_async(F&& func, Args&&... args)
        -> AsyncResult<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;
        auto fut = submit(std::forward<F>(func), std::forward<Args>(args)...);
        return AsyncResult<R>(std::move(fut), this);
    }

    /// Request graceful shutdown.  Workers finish current + queued tasks.
    void shutdown();

    /// Number of worker threads.
    [[nodiscard]] std::size_t thread_count() const noexcept {
        return workers_.size();
    }

    /// Approximate number of pending tasks (racy but useful for monitoring).
    [[nodiscard]] std::size_t pending_tasks() const {
        return queue_.size_approx();
    }

    /// Return an awaitable that, when co_await'd, moves the coroutine
    /// onto a pool worker thread.
    ///
    /// Usage inside a coroutine:
    ///   co_await pool.schedule();   // now running on a pool thread
    PoolScheduler schedule();

private:
    /// The worker loop — each jthread runs this.
    void worker_loop(std::stop_token stoken);

    TaskQueue                   queue_;
    std::vector<std::jthread>   workers_;
};

} // namespace ate

// Include AsyncResult AFTER ThreadPool is fully defined.
#include <ate/async_result.hpp>


