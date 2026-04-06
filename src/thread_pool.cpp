/// @file thread_pool.cpp
/// @brief ThreadPool implementation — constructor, destructor, worker loop.

#include <ate/thread_pool.hpp>

#include <algorithm>

namespace ate {

// ─── Constructor ────────────────────────────────────────────────────
//
// Why we clamp to ≥ 1:
//   std::thread::hardware_concurrency() can legally return 0 when the
//   value is "not computable or well-defined".  A pool with 0 threads
//   is useless, so we guarantee at least 1.
//
// Why we use std::jthread:
//   Each jthread gets its own stop_source.  We pass the stop_token
//   into worker_loop so the worker can check it inside wait_and_pop.
//   On destruction, jthread calls request_stop() then join() — we
//   never forget to join.

ThreadPool::ThreadPool(std::size_t num_threads)
{
    if (num_threads == 0) {
        num_threads = std::max(1U, std::thread::hardware_concurrency());
    }

    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        // jthread passes its stop_token as the first argument to the
        // callable IF the callable accepts one.  Our worker_loop does.
        workers_.emplace_back([this](std::stop_token st) {
            worker_loop(std::move(st));
        });
    }
}

// ─── Destructor ─────────────────────────────────────────────────────
//
// Step 1: Tell the queue to stop accepting tasks and wake all waiters.
// Step 2: Request stop on every jthread (fires their stop_tokens).
// Step 3: jthread destructor joins — waits for worker_loop to return.
//
// Why shutdown() first?
//   If a worker is blocked in wait_and_pop(), the stop_token alone
//   can wake it (condition_variable_any + stop_token).  But we also
//   need the queue to reject new pushes, so we call queue_.shutdown()
//   to set the shutdown flag and notify_all().

ThreadPool::~ThreadPool()
{
    shutdown();
}

// ─── shutdown() ─────────────────────────────────────────────────────

void ThreadPool::shutdown()
{
    queue_.shutdown();

    for (auto& w : workers_) {
        if (w.joinable()) {
            w.request_stop();
        }
    }

    // jthread destructors will join.  We don't need to do it manually,
    // but the workers must have exited their loop by the time we
    // get here.  request_stop + queue shutdown guarantees that.
}

// ─── Worker Loop ────────────────────────────────────────────────────
//
// Each worker runs this in a dedicated thread.
//
// Key correctness property:
//   The worker does NOT hold the queue mutex while invoking the task.
//   wait_and_pop() acquires the mutex, pops, releases, and returns.
//   Then we invoke the task outside any lock.
//
// Exception safety:
//   If the task throws, the packaged_task captures the exception and
//   stores it in the promise.  The caller sees it when they call
//   future::get().  The worker loop itself does not crash.
//
// Drain behaviour:
//   After stop is requested, the worker keeps popping tasks from the
//   queue (via try_pop) until it's empty.  This ensures submitted
//   work is completed.  If you want "drop all", remove the drain loop.

void ThreadPool::worker_loop(std::stop_token stoken)
{
    // ── Main loop: block waiting for tasks ──
    while (!stoken.stop_requested()) {
        auto task = queue_.wait_and_pop(stoken);
        if (task) {
            (*task)();   // Execute outside any lock
        }
    }

    // ── Drain loop: finish remaining queued tasks after stop ──
    while (auto task = queue_.try_pop()) {
        (*task)();
    }
}

} // namespace ate
