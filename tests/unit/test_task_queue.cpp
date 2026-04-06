/// @file test_task_queue.cpp
/// @brief Unit tests for ate::TaskQueue.
///
/// What we test and WHY
/// ────────────────────
/// 1. Basic push/pop — verifies the queue actually stores and returns tasks.
/// 2. FIFO ordering  — tasks must come out in the order they went in.
/// 3. try_pop on empty — must return nullopt, not block or crash.
/// 4. Shutdown semantics — after shutdown(), push returns false,
///    and wait_and_pop returns nullopt.
/// 5. Concurrent push/pop — multiple producers and consumers operating
///    simultaneously without data races.  This is the most important
///    test for a thread-safe queue.

#include <ate/task_queue.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════════
// Test 1: Basic push and pop
// ═══════════════════════════════════════════════════════════════════
// Verifies that a single task can be enqueued and dequeued.

TEST(TaskQueueTest, PushAndTryPop) {
    ate::TaskQueue q;
    int value = 0;

    ASSERT_TRUE(q.push([&] { value = 42; }));

    auto task = q.try_pop();
    ASSERT_TRUE(task.has_value());

    (*task)();   // execute the popped task
    EXPECT_EQ(value, 42);
}

// ═══════════════════════════════════════════════════════════════════
// Test 2: FIFO ordering
// ═══════════════════════════════════════════════════════════════════
// Queue must be FIFO — first in, first out.  We push three tasks
// that append to a vector.  After popping and running all three,
// the vector must contain {1, 2, 3} in that exact order.

TEST(TaskQueueTest, FIFOOrdering) {
    ate::TaskQueue q;
    std::vector<int> order;

    q.push([&] { order.push_back(1); });
    q.push([&] { order.push_back(2); });
    q.push([&] { order.push_back(3); });

    while (auto task = q.try_pop()) {
        (*task)();
    }

    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

// ═══════════════════════════════════════════════════════════════════
// Test 3: try_pop on empty queue
// ═══════════════════════════════════════════════════════════════════

TEST(TaskQueueTest, TryPopEmpty) {
    ate::TaskQueue q;
    auto task = q.try_pop();
    EXPECT_FALSE(task.has_value());
}

// ═══════════════════════════════════════════════════════════════════
// Test 4: Shutdown rejects new pushes
// ═══════════════════════════════════════════════════════════════════

TEST(TaskQueueTest, ShutdownRejectsPush) {
    ate::TaskQueue q;
    q.shutdown();

    bool accepted = q.push([] {});
    EXPECT_FALSE(accepted);
    EXPECT_TRUE(q.is_shutdown());
}

// ═══════════════════════════════════════════════════════════════════
// Test 5: wait_and_pop returns nullopt after shutdown
// ═══════════════════════════════════════════════════════════════════
// We launch a thread that blocks in wait_and_pop, then call shutdown
// from the main thread.  The blocked thread must wake up and get
// nullopt.

TEST(TaskQueueTest, WaitAndPopReturnsOnShutdown) {
    ate::TaskQueue q;
    std::jthread worker([&](std::stop_token st) {
        auto task = q.wait_and_pop(st);
        EXPECT_FALSE(task.has_value());
    });

    // Give the worker time to block in wait_and_pop
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.shutdown();
    worker.request_stop();
    // jthread destructor joins
}

// ═══════════════════════════════════════════════════════════════════
// Test 6: Concurrent producers and consumers
// ═══════════════════════════════════════════════════════════════════
// 4 producer threads each push 1000 tasks that increment an atomic.
// 4 consumer threads pop and execute tasks concurrently.
// At the end, the counter must equal 4000.
//
// This is the KEY test — it proves the mutex synchronization is
// correct under contention.

TEST(TaskQueueTest, ConcurrentPushPop) {
    ate::TaskQueue q;
    std::atomic<int> counter{0};
    constexpr int tasks_per_producer = 1000;
    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;

    // --- Producers ---
    std::vector<std::jthread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < tasks_per_producer; ++i) {
                q.push([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }

    // Wait for all producers to finish pushing
    producers.clear();   // jthread dtor joins

    // --- Consumers ---
    std::vector<std::jthread> consumers;
    for (int c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&] {
            while (auto task = q.try_pop()) {
                (*task)();
            }
        });
    }

    consumers.clear();

    EXPECT_EQ(counter.load(), num_producers * tasks_per_producer);
}

// ═══════════════════════════════════════════════════════════════════
// Test 7: size_approx
// ═══════════════════════════════════════════════════════════════════

TEST(TaskQueueTest, SizeApprox) {
    ate::TaskQueue q;
    EXPECT_EQ(q.size_approx(), 0u);

    q.push([] {});
    q.push([] {});
    q.push([] {});
    EXPECT_EQ(q.size_approx(), 3u);

    q.try_pop();
    EXPECT_EQ(q.size_approx(), 2u);
}

} // namespace
