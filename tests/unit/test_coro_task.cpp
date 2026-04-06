/// @file test_coro_task.cpp
/// @brief Unit tests for ate::CoroTask<T> and coroutine-pool integration.
///
/// What we test and WHY
/// ────────────────────
/// 1. Basic coroutine  — CoroTask<int> with co_return, get result.
/// 2. Void coroutine   — CoroTask<void> with co_return.
/// 3. Pool scheduling  — co_await pool.schedule() moves to pool thread.
/// 4. Exception in coro — unhandled_exception captures it; get() re-throws.
/// 5. Multiple coros    — many coroutines running concurrently on pool.
/// 6. Coroutine chain  — one coroutine awaits the result of pool.submit.

#include <ate/coro_task.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════════
// Test 1: Basic coroutine with co_return
// ═══════════════════════════════════════════════════════════════════

ate::CoroTask<int> simple_coro() {
    co_return 42;
}

TEST(CoroTaskTest, BasicCoReturn) {
    auto task = simple_coro();
    EXPECT_EQ(task.get(), 42);
}

// ═══════════════════════════════════════════════════════════════════
// Test 2: Void coroutine
// ═══════════════════════════════════════════════════════════════════

ate::CoroTask<void> void_coro(int& out) {
    out = 99;
    co_return;
}

TEST(CoroTaskTest, VoidCoroutine) {
    int value = 0;
    auto task = void_coro(value);
    task.get();  // resumes and completes
    EXPECT_EQ(value, 99);
}

// ═══════════════════════════════════════════════════════════════════
// Test 3: co_await pool.schedule() moves to pool thread
// ═══════════════════════════════════════════════════════════════════

ate::CoroTask<std::thread::id> pool_coro(ate::ThreadPool& pool) {
    // Before schedule: we're on the test's main thread.
    co_await pool.schedule();
    // After schedule: we're on a pool worker thread.
    co_return std::this_thread::get_id();
}

TEST(CoroTaskTest, ScheduleOnPool) {
    ate::ThreadPool pool(4);
    auto main_id = std::this_thread::get_id();

    auto task = pool_coro(pool);
    auto worker_id = task.get();

    // The coroutine ran on a different thread (a pool worker)
    EXPECT_NE(worker_id, main_id);
}

// ═══════════════════════════════════════════════════════════════════
// Test 4: Exception propagation in coroutine
// ═══════════════════════════════════════════════════════════════════

ate::CoroTask<int> throwing_coro() {
    throw std::runtime_error("coro failed");
    co_return 0;  // never reached
}

TEST(CoroTaskTest, ExceptionPropagation) {
    auto task = throwing_coro();
    EXPECT_THROW(task.get(), std::runtime_error);
}

// ═══════════════════════════════════════════════════════════════════
// Test 5: Multiple coroutines on pool
// ═══════════════════════════════════════════════════════════════════

ate::CoroTask<int> compute_on_pool(ate::ThreadPool& pool, int input) {
    co_await pool.schedule();
    co_return input * input;
}

TEST(CoroTaskTest, MultipleCoros) {
    ate::ThreadPool pool(4);
    constexpr int N = 50;

    std::vector<ate::CoroTask<int>> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        tasks.push_back(compute_on_pool(pool, i));
    }

    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(tasks[static_cast<std::size_t>(i)].get(), i * i);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test 6: Coroutine with pool computation
// ═══════════════════════════════════════════════════════════════════

ate::CoroTask<std::string> string_coro(ate::ThreadPool& pool) {
    co_await pool.schedule();
    std::string result = "Hello";
    result += " World";
    co_return result;
}

TEST(CoroTaskTest, StringResult) {
    ate::ThreadPool pool(2);
    auto task = string_coro(pool);
    EXPECT_EQ(task.get(), "Hello World");
}

// ═══════════════════════════════════════════════════════════════════
// Test 7: Move semantics — CoroTask is move-only
// ═══════════════════════════════════════════════════════════════════

TEST(CoroTaskTest, MoveSemantics) {
    auto task1 = simple_coro();
    auto task2 = std::move(task1);

    // task1 is now empty, task2 holds the coroutine
    EXPECT_TRUE(task1.done());   // empty handle reports done
    EXPECT_EQ(task2.get(), 42);
}

// ═══════════════════════════════════════════════════════════════════
// Test 8: done() check
// ═══════════════════════════════════════════════════════════════════

TEST(CoroTaskTest, DoneCheck) {
    auto task = simple_coro();
    EXPECT_FALSE(task.done());  // lazy — not started yet
    task.resume();
    EXPECT_TRUE(task.done());   // completed after resume
}

} // namespace
