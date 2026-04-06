#pragma once
/// @file asio_context.hpp
/// @brief Integration layer between Boost.Asio and our ThreadPool.
///
/// ═══════════════════════════════════════════════════════════════════
///  WHY INTEGRATE ASIO WITH OUR POOL?
/// ═══════════════════════════════════════════════════════════════════
///
/// Boost.Asio is the industry-standard C++ library for async I/O.
/// It provides:
///   • Timers (steady_timer, system_timer)
///   • TCP/UDP sockets
///   • SSL/TLS support
///   • DNS resolution
///   • Signal handling
///
/// But Asio has its own executor model (io_context + run threads).
/// Our engine already has a ThreadPool.  We don't want two separate
/// thread pools competing for CPU cores.
///
/// ── Integration Strategy ──
///
/// We dedicate a small number of threads from our pool to run
/// io_context::run().  This means Asio's completion handlers execute
/// on pool threads, sharing the same worker pool as our tasks.
///
///   ┌─────────────────────────────────────────────┐
///   │                ThreadPool                    │
///   │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐  │
///   │  │ W1  │ │ W2  │ │ W3  │ │ W4  │ │ W5  │  │
///   │  │task │ │task │ │asio │ │task │ │asio │  │
///   │  └─────┘ └─────┘ └─────┘ └─────┘ └─────┘  │
///   │           ↑           ↑                     │
///   │           │       io_context::run()          │
///   │           │       handles timers,            │
///   │           │       sockets, etc.              │
///   └─────────────────────────────────────────────┘
///
/// ── AsioContext Lifecycle ──
///
///   1. Construct with reference to ThreadPool + number of IO threads.
///   2. AsioContext submits io_context::run() tasks to the pool.
///   3. A work_guard keeps io_context alive even when idle.
///   4. On destruction: reset the work guard → io_context stops when
///      all pending handlers finish → pool threads return.
///
/// ── Usage ──
///
///   ThreadPool pool(8);
///   AsioContext asio(pool, 2);   // 2 pool threads run io_context
///
///   // Schedule a timer:
///   auto result = asio.async_after(100ms, [] { return 42; });
///   int val = result.get();  // == 42, after 100ms
///
///   // In a coroutine:
///   CoroTask<void> my_coro(AsioContext& asio, ThreadPool& pool) {
///       co_await pool.schedule();
///       co_await asio.delay(500ms);    // non-blocking sleep
///       fmt::print("500ms later!\n");
///   }

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include <ate/thread_pool.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace ate {

// ═══════════════════════════════════════════════════════════════════
//  DelayAwaiter — co_await asio.delay(duration) support
// ═══════════════════════════════════════════════════════════════════
/// Makes a Boost.Asio timer co_await-able from our coroutines.
///
/// When a coroutine does `co_await asio.delay(100ms)`:
///   1. await_ready() → false (always suspend)
///   2. await_suspend() → starts a timer; on expiry, resumes the coroutine
///   3. await_resume() → returns void (timer completed)

struct DelayAwaiter {
    boost::asio::io_context& io_ctx;
    std::chrono::steady_clock::duration duration;

    // We need the timer to live until the callback fires.
    // shared_ptr ensures it isn't destroyed when await_suspend returns.
    std::shared_ptr<boost::asio::steady_timer> timer;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        timer = std::make_shared<boost::asio::steady_timer>(io_ctx, duration);
        timer->async_wait([h](const boost::system::error_code&) mutable {
            h.resume();
        });
    }

    void await_resume() const noexcept {}
};

// ═══════════════════════════════════════════════════════════════════
//  AsioContext
// ═══════════════════════════════════════════════════════════════════

class AsioContext {
public:
    /// Create an AsioContext that runs Asio on @p io_threads of the pool.
    ///
    /// @param pool        The ThreadPool to schedule IO thread work on.
    /// @param io_threads  How many pool threads should run io_context.
    ///                    Typically 1-2 for timer/signal work;
    ///                    more for high-throughput socket I/O.
    AsioContext(ThreadPool& pool, std::size_t io_threads = 1)
        : pool_(pool)
        , io_ctx_()
        , work_guard_(boost::asio::make_work_guard(io_ctx_))
    {
        // Submit io_context::run() to the pool.
        // Each call blocks until io_context stops, so they consume
        // pool threads for the lifetime of this AsioContext.
        // We save the futures so the destructor can wait for them.
        for (std::size_t i = 0; i < io_threads; ++i) {
            io_futures_.push_back(
                pool_.submit([this] { io_ctx_.run(); })
            );
        }
    }

    /// Stop the io_context and WAIT for all io_context::run() threads
    /// to return before destroying io_ctx_.
    ///
    /// Without waiting, pool threads would still be inside io_ctx_.run()
    /// when io_ctx_ is destroyed → use-after-free → segfault.
    ~AsioContext() {
        work_guard_.reset();  // allow io_context to stop when idle
        io_ctx_.stop();       // interrupt any blocked handlers

        // Wait for all io_context::run() calls to return.
        for (auto& f : io_futures_) {
            if (f.valid()) {
                f.wait();
            }
        }
    }

    // Non-copyable, non-movable
    AsioContext(const AsioContext&) = delete;
    AsioContext& operator=(const AsioContext&) = delete;
    AsioContext(AsioContext&&) = delete;
    AsioContext& operator=(AsioContext&&) = delete;

    /// Access the underlying io_context (for custom Asio operations).
    boost::asio::io_context& get_io_context() noexcept { return io_ctx_; }

    // ─── Timer-based async ──────────────────────────────────────

    /// Run a callable after a delay, returning a future with the result.
    ///
    /// The callable runs on an Asio thread (which is a pool thread).
    /// This is useful for delayed task scheduling.
    ///
    /// Example:
    ///   auto fut = asio.async_after(100ms, [] { return 42; });
    ///   int val = fut.get();  // blocks ~100ms, then returns 42
    template <typename Rep, typename Period, typename F>
    auto async_after(std::chrono::duration<Rep, Period> delay, F&& func)
        -> std::future<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;

        auto prom = std::make_shared<std::promise<R>>();
        auto fut  = prom->get_future();

        // Timer must be shared so it lives until the callback fires.
        auto timer = std::make_shared<boost::asio::steady_timer>(io_ctx_, delay);

        timer->async_wait(
            [timer, prom, f = std::forward<F>(func)]
            (const boost::system::error_code& ec) mutable {
                if (ec) {
                    prom->set_exception(
                        std::make_exception_ptr(
                            std::runtime_error("Timer error: " + ec.message())
                        )
                    );
                    return;
                }
                try {
                    if constexpr (std::is_void_v<R>) {
                        f();
                        prom->set_value();
                    } else {
                        prom->set_value(f());
                    }
                } catch (...) {
                    prom->set_exception(std::current_exception());
                }
            }
        );

        return fut;
    }

    /// Post a callable to the io_context for immediate execution
    /// on an Asio thread (no delay).
    ///
    /// This is useful for work that MUST run on an Asio thread
    /// (e.g., socket operations that need the io_context's strand).
    template <typename F>
    auto post(F&& func) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;

        auto prom = std::make_shared<std::promise<R>>();
        auto fut  = prom->get_future();

        boost::asio::post(io_ctx_,
            [prom, f = std::forward<F>(func)]() mutable {
                try {
                    if constexpr (std::is_void_v<R>) {
                        f();
                        prom->set_value();
                    } else {
                        prom->set_value(f());
                    }
                } catch (...) {
                    prom->set_exception(std::current_exception());
                }
            }
        );

        return fut;
    }

    // ─── Coroutine integration ──────────────────────────────────

    /// co_await-able delay for coroutines.
    ///
    /// Usage:
    ///   co_await asio.delay(100ms);
    template <typename Rep, typename Period>
    DelayAwaiter delay(std::chrono::duration<Rep, Period> d) {
        return DelayAwaiter{
            io_ctx_,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(d),
            nullptr
        };
    }

private:
    ThreadPool& pool_;
    boost::asio::io_context io_ctx_;
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> work_guard_;
    std::vector<std::future<void>> io_futures_;  // track io_context::run() calls
};

} // namespace ate
