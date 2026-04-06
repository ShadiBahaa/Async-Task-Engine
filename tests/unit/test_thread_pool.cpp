/// @file test_thread_pool.cpp
/// @brief Unit tests for ate::ThreadPool.
///
/// What we test and WHY
/// ────────────────────
/// 1. Basic submit  — verifies a task runs and produces a result.
/// 2. Multiple tasks — ensures the pool can process many tasks.
/// 3. Exception propagation — a throwing task must surface the
///    exception through the future, not crash the pool.
/// 4. Return value through future — different return types work.
/// 5. Thread count — construction honours the requested count.
/// 6. Concurrent stress — many tasks across many threads; the
///    atomic counter must match the expected sum exactly.
/// 7. Shutdown behaviour — after shutdown, submit throws.

#include <ate/thread_pool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════════
// Test 1: Basic submit and future::get()
// ═══════════════════════════════════════════════════════════════════

TEST(ThreadPoolTest, BasicSubmit) {
    ate::ThreadPool pool(2);

    auto future = pool.submit([] { return 42; });
    EXPECT_EQ(future.get(), 42);
}

// ═══════════════════════════════════════════════════════════════════
// Test 2: Multiple tasks
// ═══════════════════════════════════════════════════════════════════

TEST(ThreadPoolTest, MultipleTasks) {
    ate::ThreadPool pool(4);

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([i] { return i * i; }));
    }

    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(futures[static_cast<std::size_t>(i)].get(), i * i);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test 3: Exception propagation
// ═══════════════════════════════════════════════════════════════════
// The pool must NOT crash when a task throws.  Instead, the
// exception must be captured in the promise and re-thrown when
// the caller calls future::get().

TEST(ThreadPoolTest, ExceptionPropagation) {
    ate::ThreadPool pool(2);

    auto future = pool.submit([]() -> int {
        throw std::runtime_error("boom");
    });

    EXPECT_THROW(future.get(), std::runtime_error);
}

// ═══════════════════════════════════════════════════════════════════
// Test 4: Void return type
// ═══════════════════════════════════════════════════════════════════

TEST(ThreadPoolTest, VoidReturn) {
    ate::ThreadPool pool(2);
    std::atomic<bool> ran{false};

    auto future = pool.submit([&ran] { ran.store(true); });
    future.get();

    EXPECT_TRUE(ran.load());
}

// ═══════════════════════════════════════════════════════════════════
// Test 5: Submit with arguments
// ═══════════════════════════════════════════════════════════════════

TEST(ThreadPoolTest, SubmitWithArgs) {
    ate::ThreadPool pool(2);

    auto future = pool.submit([](int a, int b) { return a + b; }, 10, 32);
    EXPECT_EQ(future.get(), 42);
}

// ═══════════════════════════════════════════════════════════════════
// Test 6: Thread count
// ═══════════════════════════════════════════════════════════════════

TEST(ThreadPoolTest, ThreadCount) {
    ate::ThreadPool pool(8);
    EXPECT_EQ(pool.thread_count(), 8u);
}

TEST(ThreadPoolTest, DefaultThreadCount) {
    ate::ThreadPool pool;
    EXPECT_GE(pool.thread_count(), 1u);
}

// ═══════════════════════════════════════════════════════════════════
// Test 7: Concurrent stress test
// ═══════════════════════════════════════════════════════════════════
// Submit 10,000 tasks, each incrementing an atomic counter.
// The final count must be exactly 10,000.  This validates that
// no tasks are lost or double-executed.

TEST(ThreadPoolTest, StressTest) {
    ate::ThreadPool pool(8);
    std::atomic<int> counter{0};
    constexpr int num_tasks = 10'000;

    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    // Wait for all tasks to complete
    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), num_tasks);
}

// ═══════════════════════════════════════════════════════════════════
// Test 8: Submit after shutdown throws
// ═══════════════════════════════════════════════════════════════════

TEST(ThreadPoolTest, SubmitAfterShutdown) {
    ate::ThreadPool pool(2);
    pool.shutdown();

    EXPECT_THROW(pool.submit([] { return 1; }), std::runtime_error);
}

// ═══════════════════════════════════════════════════════════════════
// Test 9: Tasks with move-only captures
// ═══════════════════════════════════════════════════════════════════
// This validates that our use of move_only_function works correctly
// with move-only types like unique_ptr.

TEST(ThreadPoolTest, MoveOnlyCapture) {
    ate::ThreadPool pool(2);

    auto ptr = std::make_unique<int>(42);
    auto future = pool.submit([p = std::move(ptr)]() {
        return *p;
    });

    EXPECT_EQ(future.get(), 42);
}

} // namespace
