#pragma once
/// @file coro_task.hpp
/// @brief CoroTask<T> — lazy coroutine return type for the async task engine.
///
/// ═══════════════════════════════════════════════════════════════════
///  C++20 COROUTINE PRIMER (so you can follow the code)
/// ═══════════════════════════════════════════════════════════════════
///
/// A coroutine is a function that can suspend and resume.  When the
/// compiler sees `co_return`, `co_yield`, or `co_await` in a function,
/// it transforms the function into a state machine.
///
/// The return type of a coroutine must define a nested `promise_type`.
/// The compiler calls methods on promise_type at specific points:
///
///   1. get_return_object() — creates the CoroTask that the caller sees
///   2. initial_suspend()   — should we start running immediately or lazily?
///   3. final_suspend()     — what happens when the coroutine finishes?
///   4. return_value(v)     — called on `co_return v;`
///   5. unhandled_exception() — called if the coroutine body throws
///
/// ── Lazy vs Eager ──
///   initial_suspend() returns suspend_always → LAZY (our choice)
///   The coroutine doesn't start until someone explicitly resumes it.
///   This is important because we want to schedule it on the pool.
///
/// ── Why suspend at final_suspend? ──
///   If final_suspend returns suspend_always, the coroutine frame is NOT
///   destroyed automatically.  The OWNER (CoroTask) destroys it in its
///   destructor.  This lets us safely access the result after completion.
///
/// ── How get() works with pool scheduling ──
///   When a coroutine does `co_await pool.schedule()`, the main thread
///   can't simply spin-resume — the pool owns the resumption.
///   Instead, we attach a std::promise to the coroutine's promise_type
///   at final_suspend time (via a custom FinalAwaiter).  The FinalAwaiter
///   fulfills the promise, and get() blocks on the corresponding future.
///
/// ═══════════════════════════════════════════════════════════════════
///  USAGE EXAMPLE
/// ═══════════════════════════════════════════════════════════════════
///
///   CoroTask<int> compute(ThreadPool& pool) {
///       co_await pool.schedule();        // move to pool thread
///       int result = heavy_compute();
///       co_return result;
///   }
///
///   // To get the result:
///   auto task = compute(pool);
///   int value = task.get();  // resumes + blocks until done

#include <coroutine>
#include <exception>
#include <future>
#include <optional>
#include <type_traits>
#include <utility>

namespace ate {

// Forward declarations
class ThreadPool;

template <typename T = void>
class CoroTask;

// ═══════════════════════════════════════════════════════════════════
//  FinalAwaiter — signals completion at the end of the coroutine
// ═══════════════════════════════════════════════════════════════════
//  FinalAwaiter — signals completion AFTER the coroutine suspends
// ═══════════════════════════════════════════════════════════════════
//
// WHY await_suspend() and NOT final_suspend()?
//
// final_suspend() runs while the coroutine is still "active".
// If we signal the future there, the main thread calls fut.get(),
// then ~CoroTask() → handle_.destroy() — but the pool thread is
// still inside final_suspend()!  That's a use-after-free.
//
// await_suspend() runs AFTER the coroutine is fully suspended.
// At that point the frame is in a quiescent state, and it's safe
// for another thread to destroy it.

/// FinalAwaiter for CoroTask<T> (non-void).
template <typename T>
struct FinalAwaiterVal {
    std::optional<T>*   result;
    std::exception_ptr* exception;
    std::promise<T>**   ext_promise;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<>) const noexcept {
        // Coroutine is NOW fully suspended — safe to signal.
        if (*ext_promise) {
            if (*exception) {
                (*ext_promise)->set_exception(*exception);
            } else {
                (*ext_promise)->set_value(std::move(**result));
            }
        }
    }

    void await_resume() const noexcept {}
};

/// FinalAwaiter for CoroTask<void>.
struct FinalAwaiterVoid {
    bool*               completed;
    std::exception_ptr* exception;
    std::promise<void>** ext_promise;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<>) const noexcept {
        if (*ext_promise) {
            if (*exception) {
                (*ext_promise)->set_exception(*exception);
            } else {
                (*ext_promise)->set_value();
            }
        }
    }

    void await_resume() const noexcept {}
};

// ═══════════════════════════════════════════════════════════════════
//  Promise Type for CoroTask<T>  (T != void)
// ═══════════════════════════════════════════════════════════════════

namespace detail {

template <typename T>
struct CoroPromise {
    /// Create the CoroTask object that the caller receives.
    CoroTask<T> get_return_object();

    /// LAZY: suspend immediately — don't run until explicitly resumed.
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Signal completion in await_suspend (after coroutine is suspended).
    FinalAwaiterVal<T> final_suspend() noexcept {
        return { &result_, &exception_, &external_promise_ };
    }

    /// Store the co_return'd value.
    void return_value(T value) {
        result_.emplace(std::move(value));
    }

    /// Capture any exception thrown inside the coroutine.
    void unhandled_exception() {
        exception_ = std::current_exception();
    }

    /// Retrieve the result synchronously (only safe if coroutine is done).
    T get_result() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        return std::move(*result_);
    }

    /// Check if the coroutine has produced a result.
    bool has_result() const noexcept {
        return result_.has_value() || exception_ != nullptr;
    }

    /// Attach an external promise for future-based blocking.
    void set_external_promise(std::promise<T>* p) { external_promise_ = p; }

private:
    std::optional<T>       result_;
    std::exception_ptr     exception_;
    std::promise<T>*       external_promise_ = nullptr;
};

// ═══════════════════════════════════════════════════════════════════
//  Promise Type for CoroTask<void>
// ═══════════════════════════════════════════════════════════════════

template <>
struct CoroPromise<void> {
    CoroTask<void> get_return_object();

    std::suspend_always initial_suspend() noexcept { return {}; }

    FinalAwaiterVoid final_suspend() noexcept {
        return { &completed_, &exception_, &external_promise_ };
    }

    void return_void() {
        completed_ = true;
    }

    void unhandled_exception() {
        exception_ = std::current_exception();
    }

    void get_result() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    bool has_result() const noexcept {
        return completed_ || exception_ != nullptr;
    }

    void set_external_promise(std::promise<void>* p) { external_promise_ = p; }

private:
    bool                   completed_ = false;
    std::exception_ptr     exception_;
    std::promise<void>*    external_promise_ = nullptr;
};

} // namespace detail

// ═══════════════════════════════════════════════════════════════════
//  CoroTask<T>
// ═══════════════════════════════════════════════════════════════════
///
/// get() works as follows:
///   1. Attach a std::promise to the coroutine's promise_type.
///   2. Resume the coroutine (starts it running).
///   3. Block on the corresponding std::future.
///
/// If the coroutine never suspends (no co_await pool.schedule()),
/// resume() runs it to completion synchronously, the FinalAwaiter
/// signals the promise, and future.get() returns immediately.
///
/// If the coroutine suspends via co_await pool.schedule(), resume()
/// runs it until the first co_await.  The pool takes over, and
/// when the coroutine eventually completes, the FinalAwaiter signals
/// the promise from the pool thread.

template <typename T>
class CoroTask {
public:
    using promise_type = detail::CoroPromise<T>;
    using handle_type  = std::coroutine_handle<promise_type>;

    explicit CoroTask(handle_type h) : handle_(h) {}

    /// Move-only (coroutine handles are unique ownership).
    CoroTask(CoroTask&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    CoroTask& operator=(CoroTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    CoroTask(const CoroTask&) = delete;
    CoroTask& operator=(const CoroTask&) = delete;

    /// Destroy the coroutine frame.
    ~CoroTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    /// Resume the coroutine (runs until next suspend point or completion).
    void resume() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
        }
    }

    /// Check if the coroutine has finished.
    [[nodiscard]] bool done() const noexcept {
        return handle_ ? handle_.done() : true;
    }

    /// Get the result.  This is the primary user-facing API.
    ///
    /// Works for both synchronous coroutines (no pool) and
    /// pool-scheduled coroutines (co_await pool.schedule()).
    ///
    ///  1. Attach a std::promise to get notified on completion.
    ///  2. Resume the coroutine (starts it / runs to first co_await).
    ///  3. Block on the future until the coroutine finishes.
    T get() {
        if (done()) {
            // Already completed — just get the result directly.
            return handle_.promise().get_result();
        }

        // Attach a promise/future pair for thread-safe signalling.
        std::promise<T> prom;
        auto fut = prom.get_future();
        handle_.promise().set_external_promise(&prom);

        // Resume: runs synchronously until the coroutine suspends
        // (at co_await or at final_suspend if it completes).
        resume();

        // Block until the FinalAwaiter fulfills the promise.
        return fut.get();
    }

    /// Get the underlying handle (for advanced use / pool scheduling).
    handle_type handle() const noexcept { return handle_; }

private:
    handle_type handle_;
};

// ═══════════════════════════════════════════════════════════════════
//  get_return_object implementations
// ═══════════════════════════════════════════════════════════════════

namespace detail {

template <typename T>
CoroTask<T> CoroPromise<T>::get_return_object() {
    return CoroTask<T>{
        std::coroutine_handle<CoroPromise<T>>::from_promise(*this)
    };
}

inline CoroTask<void> CoroPromise<void>::get_return_object() {
    return CoroTask<void>{
        std::coroutine_handle<CoroPromise<void>>::from_promise(*this)
    };
}

} // namespace detail

// ═══════════════════════════════════════════════════════════════════
//  PoolScheduler — an awaitable that moves execution to the pool
// ═══════════════════════════════════════════════════════════════════
///
/// Usage:   co_await pool.schedule();
///
/// When a coroutine hits `co_await pool.schedule()`:
///   1. await_ready() returns false → the coroutine will suspend.
///   2. await_suspend(handle) is called with the coroutine's handle.
///      We submit a task to the pool that resumes the handle.
///      The coroutine continues on a POOL WORKER thread.
///   3. await_resume() has nothing to return (void).

struct PoolScheduler {
    ThreadPool* pool;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) const;

    void await_resume() const noexcept {}
};

} // namespace ate

// Include ThreadPool AFTER CoroTask and PoolScheduler are defined.
#include <ate/thread_pool.hpp>

namespace ate {

/// Submit a coroutine handle to the pool for resumption.
inline void PoolScheduler::await_suspend(std::coroutine_handle<> h) const {
    pool->submit([h]() mutable { h.resume(); });
}

/// ThreadPool::schedule() is defined here (not in thread_pool.hpp) because
/// it returns PoolScheduler by value, which requires the type to be complete.
inline PoolScheduler ThreadPool::schedule() {
    return PoolScheduler{this};
}

} // namespace ate
