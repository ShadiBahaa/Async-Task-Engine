#pragma once
/// @file task_queue.hpp
/// @brief Thread-safe MPMC task queue (mutex + condition_variable_any).
///
/// Design notes
/// ────────────
/// • Uses std::condition_variable_any so we can pass a stop_token into wait().
///   This lets workers wake up automatically when the pool requests a stop —
///   no need for a separate "shutdown" flag or poison pill.
///
/// • The queue stores std::move_only_function<void()> (C++23).  This avoids
///   the overhead of std::function's internal copy requirement and allows
///   move-only captures (e.g. std::promise, std::unique_ptr).
///
/// • push() returns bool — false if the queue has been shut down, so the
///   caller knows the task was NOT enqueued.
///
/// • try_pop() is non-blocking; wait_and_pop() blocks until a task arrives
///   OR a stop is requested.
///
/// Pitfalls to watch for
/// ─────────────────────
/// 1. Always notify_one/notify_all AFTER releasing the lock, or at least
///    after the data is visible.  We notify inside the lock here because
///    cv_any + stop_token needs the mutex-aware path. The performance
///    cost is negligible for a mutex-based queue.
///
/// 2. Never hold the lock while executing a task — that would serialise
///    all workers and deadlock if a task re-submits.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>

namespace ate {

class TaskQueue {
public:
    /// The type-erased callable that workers execute.
    /// std::move_only_function<void()> is C++23 — if your compiler doesn't
    /// have it yet, fall back to std::function<void()>.
    using TaskType = std::move_only_function<void()>;

    TaskQueue() = default;

    // Non-copyable, non-movable (contains mutex)
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    TaskQueue(TaskQueue&&) = delete;
    TaskQueue& operator=(TaskQueue&&) = delete;

    /// Enqueue a task.  Returns false if the queue has been shut down.
    bool push(TaskType task) {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return false;
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();        // wake one waiting worker
        return true;
    }

    /// Non-blocking pop.  Returns std::nullopt if the queue is empty.
    std::optional<TaskType> try_pop() {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) return std::nullopt;
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        return task;
    }

    /// Blocking pop.  Waits until:
    ///   (a) a task is available  → returns it, or
    ///   (b) stop is requested    → returns std::nullopt.
    ///
    /// The stop_token comes from the jthread; when the pool calls
    /// request_stop(), every blocked worker wakes up and sees nullopt.
    std::optional<TaskType> wait_and_pop(std::stop_token stoken) {
        std::unique_lock lock(mutex_);

        // wait() atomically releases lock, sleeps, then re-acquires.
        // Returns false if stop was requested.
        bool got_work = cv_.wait(lock, stoken, [this] {
            return !tasks_.empty() || shutdown_;
        });

        if (!got_work || tasks_.empty()) return std::nullopt;

        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        return task;
    }

    /// Signal that no more tasks will be accepted.  Wakes all waiters.
    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    /// How many tasks are currently queued (approximate — racy but useful
    /// for diagnostics / monitoring).
    [[nodiscard]] std::size_t size_approx() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

    [[nodiscard]] bool is_shutdown() const {
        std::lock_guard lock(mutex_);
        return shutdown_;
    }

private:
    mutable std::mutex              mutex_;
    std::condition_variable_any     cv_;
    std::deque<TaskType>            tasks_;
    bool                            shutdown_{false};
};

} // namespace ate
