/// @file test_lockfree_queue.cpp
/// @brief Unit tests for ate::LockFreeQueue<T>.
///
/// Test strategy
/// ─────────────
/// Lock-free data structures are notoriously hard to test because bugs
/// are often timing-dependent.  Our approach:
///
/// 1. Sequential correctness — basic push/pop works in a single thread.
/// 2. Boundary conditions   — full queue, empty queue.
/// 3. Power-of-2 rounding   — capacity is always rounded up.
/// 4. MPMC stress test       — the most important test.
///    Multiple producers push unique values; multiple consumers pop.
///    At the end, we verify every value was dequeued exactly once.
///    If there's a race (double-pop, lost element), this catches it.
/// 5. Single-producer single-consumer (SPSC) — a simpler concurrent test.

#include <ate/lockfree_queue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════════
// Test 1: Basic push and pop
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, BasicPushPop) {
    ate::LockFreeQueue<int> q(8);

    ASSERT_TRUE(q.try_push(42));
    auto val = q.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
}

// ═══════════════════════════════════════════════════════════════════
// Test 2: FIFO ordering (single-threaded)
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, FIFOOrdering) {
    ate::LockFreeQueue<int> q(8);

    q.try_push(1);
    q.try_push(2);
    q.try_push(3);

    EXPECT_EQ(*q.try_pop(), 1);
    EXPECT_EQ(*q.try_pop(), 2);
    EXPECT_EQ(*q.try_pop(), 3);
}

// ═══════════════════════════════════════════════════════════════════
// Test 3: Pop from empty queue
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, PopEmpty) {
    ate::LockFreeQueue<int> q(4);
    EXPECT_FALSE(q.try_pop().has_value());
}

// ═══════════════════════════════════════════════════════════════════
// Test 4: Push to full queue
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, PushFull) {
    ate::LockFreeQueue<int> q(4);  // capacity = 4

    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_TRUE(q.try_push(4));
    EXPECT_FALSE(q.try_push(5));  // queue is full
}

// ═══════════════════════════════════════════════════════════════════
// Test 5: Capacity is rounded to power of 2
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, CapacityRoundsUp) {
    ate::LockFreeQueue<int> q1(5);   // → 8
    EXPECT_EQ(q1.capacity(), 8u);

    ate::LockFreeQueue<int> q2(8);   // already power of 2
    EXPECT_EQ(q2.capacity(), 8u);

    ate::LockFreeQueue<int> q3(100); // → 128
    EXPECT_EQ(q3.capacity(), 128u);
}

// ═══════════════════════════════════════════════════════════════════
// Test 6: Wrap-around — push/pop more than capacity elements
// ═══════════════════════════════════════════════════════════════════
// This tests that the ring buffer wraps correctly and slot sequence
// counters are recycled properly.

TEST(LockFreeQueueTest, WrapAround) {
    ate::LockFreeQueue<int> q(4);

    // Fill and drain 3 full rounds
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.try_push(round * 4 + i));
        }
        for (int i = 0; i < 4; ++i) {
            auto val = q.try_pop();
            ASSERT_TRUE(val.has_value());
            EXPECT_EQ(*val, round * 4 + i);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test 7: SPSC concurrent test
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, SPSCConcurrent) {
    ate::LockFreeQueue<int> q(1024);
    constexpr int count = 100'000;

    std::jthread producer([&] {
        for (int i = 0; i < count; ++i) {
            while (!q.try_push(i)) {
                // Spin — queue full
            }
        }
    });

    std::vector<int> received;
    received.reserve(count);

    std::jthread consumer([&] {
        for (int i = 0; i < count; ++i) {
            std::optional<int> val;
            while (!(val = q.try_pop())) {
                // Spin — queue empty
            }
            received.push_back(*val);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(received[static_cast<std::size_t>(i)], i);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test 8: MPMC stress test — the KEY correctness test
// ═══════════════════════════════════════════════════════════════════
// 4 producers each push unique values.  4 consumers pop.
// We verify that every value appears exactly once in the collected
// results — no duplicates, no losses.

TEST(LockFreeQueueTest, MPMCStress) {
    ate::LockFreeQueue<int> q(4096);
    constexpr int per_producer = 50'000;
    constexpr int num_producers = 4;
    constexpr int num_consumers = 4;
    constexpr int total = num_producers * per_producer;

    std::atomic<bool> done{false};

    // Each producer pushes a unique range:
    //   producer 0: [0, per_producer)
    //   producer 1: [per_producer, 2*per_producer)
    //   ...
    std::vector<std::jthread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&q, p] {
            int base = p * per_producer;
            for (int i = 0; i < per_producer; ++i) {
                while (!q.try_push(base + i)) {
                    // Spin — queue full
                }
            }
        });
    }

    // Consumers collect values into per-thread vectors.
    std::vector<std::vector<int>> per_consumer(
        static_cast<std::size_t>(num_consumers));

    std::vector<std::jthread> consumers;
    for (int c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&q, &done, &per_consumer, c] {
            auto& mine = per_consumer[static_cast<std::size_t>(c)];
            while (!done.load(std::memory_order_relaxed)) {
                if (auto val = q.try_pop()) {
                    mine.push_back(*val);
                }
            }
            // Drain remaining
            while (auto val = q.try_pop()) {
                mine.push_back(*val);
            }
        });
    }

    // Wait for producers to finish
    producers.clear();  // join all

    // Let consumers drain
    done.store(true, std::memory_order_relaxed);
    consumers.clear();  // join all

    // Merge all consumer results
    std::vector<int> all;
    for (auto& v : per_consumer) {
        all.insert(all.end(), v.begin(), v.end());
    }

    // Sort and verify: every value 0..total-1 appears exactly once
    std::sort(all.begin(), all.end());
    ASSERT_EQ(all.size(), static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        EXPECT_EQ(all[static_cast<std::size_t>(i)], i)
            << "Missing or duplicate value at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test 9: size_approx
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, SizeApprox) {
    ate::LockFreeQueue<int> q(8);

    EXPECT_EQ(q.size_approx(), 0u);
    q.try_push(1);
    q.try_push(2);
    EXPECT_EQ(q.size_approx(), 2u);
    q.try_pop();
    EXPECT_EQ(q.size_approx(), 1u);
}

// ═══════════════════════════════════════════════════════════════════
// Test 10: Move-only types
// ═══════════════════════════════════════════════════════════════════

TEST(LockFreeQueueTest, MoveOnlyType) {
    ate::LockFreeQueue<std::unique_ptr<int>> q(4);

    auto p = std::make_unique<int>(99);
    ASSERT_TRUE(q.try_push(std::move(p)));

    auto result = q.try_pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(**result, 99);
}

} // namespace
