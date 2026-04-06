/// @file test_asio_context.cpp
/// @brief Unit tests for ate::AsioContext (Boost.Asio integration).
///
/// What we test and WHY
/// ────────────────────
/// 1. async_after — delayed callable completes after specified time.
/// 2. post       — immediate execution on Asio thread.
/// 3. Multiple timers — several concurrent timers complete correctly.
/// 4. Timer with void return — void callable works.
/// 5. Coroutine delay — co_await asio.delay() in a coroutine.
/// 6. Timer cancellation — AsioContext destructor cleanly shuts down.
/// 7. Exception in handler — exception propagates through future.

#include <ate/asio_context.hpp>
#include <ate/coro_task.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

// ═══════════════════════════════════════════════════════════════════
// Test 1: async_after — delayed execution
// ═══════════════════════════════════════════════════════════════════

TEST(AsioContextTest, AsyncAfterBasic) {
    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 1);

    auto start = std::chrono::steady_clock::now();
    auto fut = asio.async_after(50ms, [] { return 42; });

    EXPECT_EQ(fut.get(), 42);

    auto elapsed = std::chrono::steady_clock::now() - start;
    // Should have taken at least 50ms
    EXPECT_GE(elapsed, 40ms);  // small margin for scheduling jitter
}

// ═══════════════════════════════════════════════════════════════════
// Test 2: post — immediate execution on Asio thread
// ═══════════════════════════════════════════════════════════════════

TEST(AsioContextTest, PostBasic) {
    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 1);

    auto fut = asio.post([] { return std::string("hello"); });
    EXPECT_EQ(fut.get(), "hello");
}

// ═══════════════════════════════════════════════════════════════════
// Test 3: Multiple concurrent timers
// ═══════════════════════════════════════════════════════════════════

TEST(AsioContextTest, MultipleConcurrentTimers) {
    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 2);  // 2 IO threads for parallelism

    auto start = std::chrono::steady_clock::now();

    // Launch 5 timers concurrently, each with 50ms delay
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(asio.async_after(50ms, [i] { return i * 10; }));
    }

    // All should complete
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(futures[static_cast<std::size_t>(i)].get(), i * 10);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    // They ran concurrently, so total time should be ~50ms, not 250ms
    EXPECT_LT(elapsed, 200ms);
}

// ═══════════════════════════════════════════════════════════════════
// Test 4: Void return from async_after
// ═══════════════════════════════════════════════════════════════════

TEST(AsioContextTest, AsyncAfterVoid) {
    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 1);

    std::atomic<bool> executed{false};
    auto fut = asio.async_after(10ms, [&executed] { executed.store(true); });

    fut.get();  // wait for completion
    EXPECT_TRUE(executed.load());
}

// ═══════════════════════════════════════════════════════════════════
// Test 5: Coroutine with co_await asio.delay()
// ═══════════════════════════════════════════════════════════════════

ate::CoroTask<int> coro_with_delay(ate::ThreadPool& pool,
                                    ate::AsioContext& asio) {
    co_await pool.schedule();          // move to pool thread
    co_await asio.delay(50ms);         // async sleep
    co_return 123;
}

TEST(AsioContextTest, CoroutineDelay) {
    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 1);

    auto start = std::chrono::steady_clock::now();

    auto task = coro_with_delay(pool, asio);
    EXPECT_EQ(task.get(), 123);

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 40ms);
}

// ═══════════════════════════════════════════════════════════════════
// Test 6: Exception propagation in handler
// ═══════════════════════════════════════════════════════════════════

TEST(AsioContextTest, ExceptionInHandler) {
    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 1);

    auto fut = asio.async_after(10ms, []() -> int {
        throw std::runtime_error("async fail");
    });

    EXPECT_THROW(fut.get(), std::runtime_error);
}

// ═══════════════════════════════════════════════════════════════════
// Test 7: Clean shutdown with pending timers
// ═══════════════════════════════════════════════════════════════════
// The AsioContext destructor should cleanly shut down even if
// there are pending timers.

TEST(AsioContextTest, CleanShutdown) {
    ate::ThreadPool pool(4);

    {
        ate::AsioContext asio(pool, 1);

        // Start long timers we DON'T wait for
        asio.async_after(10s, [] { return 0; });
        asio.async_after(10s, [] { return 0; });

        // Destructor should NOT hang
    }  // ~AsioContext runs here

    // Pool is still alive and functional
    auto fut = pool.submit([] { return 99; });
    EXPECT_EQ(fut.get(), 99);
}

// ═══════════════════════════════════════════════════════════════════
// Test 8: post runs on Asio thread (not main thread)
// ═══════════════════════════════════════════════════════════════════

TEST(AsioContextTest, PostRunsOnDifferentThread) {
    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 1);
    auto main_id = std::this_thread::get_id();

    auto fut = asio.post([main_id] {
        return std::this_thread::get_id() != main_id;
    });

    EXPECT_TRUE(fut.get());
}

} // namespace
