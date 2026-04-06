#pragma once
/// @file task.hpp
/// @brief Task metadata wrapper for the async task engine.
///
/// Design notes
/// ────────────
/// This header defines a Priority enum and a Task wrapper that bundles
/// a type-erased callable with optional metadata (priority, name).
///
/// Why a separate Task type?
/// ─────────────────────────
/// In Phase 1 we stored raw move_only_function<void()> in the queue.
/// That's fine for FIFO, but once we add priority scheduling (Scheduler),
/// we need metadata alongside the callable.  This type provides that
/// extension point WITHOUT changing the queue interface — the queue
/// now stores Task objects instead of raw callables.
///
/// We keep this lightweight:
///   - No virtual functions, no heap allocation beyond the callable itself
///   - Priority is a simple enum, not a full "task graph" node
///   - Name is optional (empty string = no overhead if unused)

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace ate {

/// Task priority levels.
/// Lower numeric value = higher priority (processed first).
enum class Priority : std::uint8_t {
    High   = 0,
    Normal = 1,
    Low    = 2
};

/// A task is a callable + metadata.
/// The queue stores these; the worker pops one and calls task().
struct Task {
    using Callable = std::move_only_function<void()>;

    Callable    callable;
    Priority    priority{Priority::Normal};
    std::string name;         // Optional — useful for logging/debugging

    /// Invoke the task.
    void operator()() { callable(); }

    /// Comparison for priority queue: lower priority value = higher urgency.
    /// This makes std::priority_queue a max-heap on urgency.
    friend bool operator<(const Task& a, const Task& b) noexcept {
        // We want High (0) to be "greater" so it gets popped first.
        // priority_queue pops the *largest* element, so we reverse:
        return a.priority > b.priority;
    }

    /// Construct from any callable.
    explicit Task(Callable fn,
                  Priority p = Priority::Normal,
                  std::string n = {})
        : callable(std::move(fn))
        , priority(p)
        , name(std::move(n))
    {}

    Task() = default;
    Task(Task&&) = default;
    Task& operator=(Task&&) = default;

    // Non-copyable (contains move_only_function)
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
};

} // namespace ate
