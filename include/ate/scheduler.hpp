#pragma once
/// @file scheduler.hpp
/// @brief Pluggable scheduling policy for the task engine.
///
/// ═══════════════════════════════════════════════════════════════════
///  WHY A SCHEDULER ABSTRACTION?
/// ═══════════════════════════════════════════════════════════════════
///
/// The ThreadPool currently uses a FIFO queue.  But real systems need:
///   • Priority scheduling (high-priority tasks run first)
///   • Fair scheduling (no starvation of low-priority tasks)
///   • Work-stealing (future: per-thread queues)
///
/// The Scheduler is a policy layer that sits between submit() and
/// the raw queue.  It decides WHERE and WHEN to place tasks.
///
/// ── Design ──
///
///   SchedulePolicy (enum):  FIFO, Priority
///
///   PriorityTask:  wraps a move_only_function + priority level
///   PriorityScheduler:  uses std::priority_queue under a mutex

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

namespace ate {

// ─── Task Priority ──────────────────────────────────────────────

enum class TaskPriority : std::uint8_t {
    Low    = 0,
    Normal = 1,
    High   = 2,
    Critical = 3
};

// ─── Priority Task ──────────────────────────────────────────────

struct PriorityTask {
    std::move_only_function<void()> func;
    TaskPriority priority = TaskPriority::Normal;
    std::uint64_t sequence = 0;  // tie-breaker for FIFO within same priority

    /// Comparison: higher priority first; same priority → lower sequence first (FIFO).
    bool operator<(const PriorityTask& other) const {
        if (priority != other.priority) {
            return static_cast<int>(priority) < static_cast<int>(other.priority);
        }
        // For std::priority_queue (max-heap), lower sequence = earlier = higher priority
        return sequence > other.sequence;
    }
};

// ─── Priority Scheduler ─────────────────────────────────────────

class PriorityScheduler {
public:
    PriorityScheduler() = default;
    ~PriorityScheduler() { shutdown(); }

    // Non-copyable, non-movable
    PriorityScheduler(const PriorityScheduler&) = delete;
    PriorityScheduler& operator=(const PriorityScheduler&) = delete;
    PriorityScheduler(PriorityScheduler&&) = delete;
    PriorityScheduler& operator=(PriorityScheduler&&) = delete;

    /// Push a task with a given priority.
    bool push(std::move_only_function<void()> func,
              TaskPriority priority = TaskPriority::Normal) {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return false;
            queue_.push(PriorityTask{
                std::move(func),
                priority,
                next_seq_++
            });
        }
        cv_.notify_one();
        return true;
    }

    /// Try to pop the highest-priority task (non-blocking).
    std::optional<PriorityTask> try_pop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        // priority_queue::top() returns const ref; we need to move
        PriorityTask task = std::move(const_cast<PriorityTask&>(queue_.top()));
        queue_.pop();
        return task;
    }

    /// Block until a task is available or shutdown is requested.
    std::optional<PriorityTask> wait_and_pop(std::stop_token stoken) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, stoken, [this] { return !queue_.empty() || stopped_; });

        if (queue_.empty()) return std::nullopt;
        PriorityTask task = std::move(const_cast<PriorityTask&>(queue_.top()));
        queue_.pop();
        return task;
    }

    /// Signal shutdown — unblock all waiters.
    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    /// Approximate size.
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex                      mutex_;
    std::condition_variable_any             cv_;
    std::priority_queue<PriorityTask>       queue_;
    std::uint64_t                           next_seq_ = 0;
    bool                                    stopped_ = false;
};

} // namespace ate
