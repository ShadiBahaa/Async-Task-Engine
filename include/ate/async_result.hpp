#pragma once
/// @file async_result.hpp
/// @brief AsyncResult<T> — a chainable future wrapper for the task engine.
///
/// Design notes
/// ────────────
/// std::future<T> is limited: you can call .get() exactly once, and there
/// is no way to attach a continuation ("do X when the result is ready").
///
/// AsyncResult<T> fixes this by adding:
///   .then(continuation)  — returns AsyncResult<U> where U = continuation(T)
///   .get()               — blocks and returns T (delegates to the inner future)
///   .wait()              — blocks until result is ready
///   .valid()             — checks if the future is valid
///
/// How .then() works
/// ─────────────────
/// 1. We spawn a new task on the pool that:
///      a. Calls this->get() to wait for the current result
///      b. Passes that result to the continuation
///      c. Returns the continuation's result (captured by a new promise)
/// 2. The new task returns AsyncResult<U>, so chains compose naturally:
///      pool.submit(f).then(g).then(h)   →   h(g(f()))
///
/// Why schedule on the pool (not inline)?
/// ──────────────────────────────────────
/// If we ran the continuation inline (on the thread that sets the promise),
/// a long chain a.then(b).then(c).then(d) would execute b, c, d all on
/// the same worker thread — starving other tasks and defeating the pool.
/// By re-submitting to the pool, we get proper load balancing.
///
/// The trade-off is an extra enqueue/dequeue per .then() stage.  For
/// CPU-heavy continuations this is negligible; for trivial ones we may
/// add a .then_inline() variant later.
///
/// Exception propagation
/// ─────────────────────
/// If the predecessor throws, get() re-throws inside the continuation's
/// task.  That exception is captured by the packaged_task and propagated
/// to the next AsyncResult.  So exceptions chain correctly:
///   pool.submit(throws).then(g)   →   g never runs; .get() re-throws.

#include <future>
#include <type_traits>
#include <utility>

namespace ate {

// Forward-declare ThreadPool so we can hold a pointer to it.
class ThreadPool;

template <typename T>
class AsyncResult {
public:
    /// Construct from a future + a reference to the pool (for .then()).
    AsyncResult(std::future<T> fut, ThreadPool* pool)
        : future_(std::move(fut))
        , pool_(pool)
    {}

    // Move-only (futures are move-only)
    AsyncResult(AsyncResult&&) = default;
    AsyncResult& operator=(AsyncResult&&) = default;
    AsyncResult(const AsyncResult&) = delete;
    AsyncResult& operator=(const AsyncResult&) = delete;

    /// Block until the result is available, then return it.
    /// Throws any exception that the task threw.
    T get() { return future_.get(); }

    /// Block until the result is ready (but don't consume it).
    void wait() const { future_.wait(); }

    /// Check if this result is associated with a valid shared state.
    [[nodiscard]] bool valid() const noexcept { return future_.valid(); }

    /// Attach a continuation: when this result is ready, invoke
    /// continuation(result) on the pool and return a new AsyncResult<U>.
    ///
    /// Usage:
    ///   auto r = pool.submit([] { return 2; })
    ///               .then([](int x) { return x * 3; })
    ///               .then([](int x) { return std::to_string(x); });
    ///   std::string s = r.get();  // "6"
    template <typename F>
    auto then(F&& continuation) -> AsyncResult<std::invoke_result_t<F, T>>;

private:
    std::future<T>  future_;
    ThreadPool*     pool_;    // non-owning; the pool must outlive its results
};

/// Specialisation for void — .then(continuation) calls continuation()
/// with no arguments.
template <>
class AsyncResult<void> {
public:
    AsyncResult(std::future<void> fut, ThreadPool* pool)
        : future_(std::move(fut))
        , pool_(pool)
    {}

    AsyncResult(AsyncResult&&) = default;
    AsyncResult& operator=(AsyncResult&&) = default;
    AsyncResult(const AsyncResult&) = delete;
    AsyncResult& operator=(const AsyncResult&) = delete;

    void get() { future_.get(); }
    void wait() const { future_.wait(); }
    [[nodiscard]] bool valid() const noexcept { return future_.valid(); }

    template <typename F>
    auto then(F&& continuation) -> AsyncResult<std::invoke_result_t<F>>;

private:
    std::future<void>  future_;
    ThreadPool*        pool_;
};

} // namespace ate

// ─── Implementation of .then() ─────────────────────────────────────
// This must come AFTER ThreadPool is fully defined, because .then()
// calls pool_->submit().  We include thread_pool.hpp here — it's safe
// because thread_pool.hpp does NOT include async_result.hpp (no cycle).

#include <ate/thread_pool.hpp>

namespace ate {

template <typename T>
template <typename F>
auto AsyncResult<T>::then(F&& continuation)
    -> AsyncResult<std::invoke_result_t<F, T>>
{
    using U = std::invoke_result_t<F, T>;

    // We need to move `this->future_` into the lambda, because
    // get() can only be called once.  After this, *this is invalid.
    auto shared = std::make_shared<std::future<T>>(std::move(future_));

    // Submit a new task that waits for us, then runs the continuation.
    // The new task returns U, so submit() gives us future<U>.
    auto fut = pool_->submit(
        [shared, cont = std::forward<F>(continuation)]() mutable -> U {
            T val = shared->get();         // Wait for predecessor
            return cont(std::move(val));   // Run continuation
        }
    );

    return AsyncResult<U>(std::move(fut), pool_);
}

template <typename F>
auto AsyncResult<void>::then(F&& continuation)
    -> AsyncResult<std::invoke_result_t<F>>
{
    using U = std::invoke_result_t<F>;

    auto shared = std::make_shared<std::future<void>>(std::move(future_));

    auto fut = pool_->submit(
        [shared, cont = std::forward<F>(continuation)]() mutable -> U {
            shared->get();       // Wait for predecessor (may throw)
            return cont();       // Run continuation
        }
    );

    return AsyncResult<U>(std::move(fut), pool_);
}

} // namespace ate
