/// @file stress_queue.cpp
/// @brief Stress tests for TaskQueue and LockFreeQueue under extreme contention.

#include <ate/lockfree_queue.hpp>
#include <ate/task_queue.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

// ─── Stress 1: Mutex queue — 8P/8C × 50K ops ───────────────────

TEST(StressQueue, MutexQueue8P8C) {
    ate::TaskQueue queue;
    constexpr int PRODUCERS = 8;
    constexpr int PER_PRODUCER = 50'000;
    std::atomic<int> consumed{0};
    const int total = PRODUCERS * PER_PRODUCER;

    std::vector<std::thread> threads;

    for (int p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < PER_PRODUCER; ++i) {
                queue.push([] {});
            }
        });
    }

    for (int c = 0; c < PRODUCERS; ++c) {
        threads.emplace_back([&] {
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (queue.try_pop().has_value()) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(consumed.load(), total);
}

// ─── Stress 2: Lock-free queue — 8P/8C × 50K ops ───────────────

TEST(StressQueue, LockFreeQueue8P8C) {
    ate::LockFreeQueue<int> queue(1024 * 128);
    constexpr int PRODUCERS = 8;
    constexpr int PER_PRODUCER = 50'000;
    std::atomic<int> consumed{0};
    const int total = PRODUCERS * PER_PRODUCER;

    std::vector<std::thread> threads;

    for (int p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&] {
            for (int i = 0; i < PER_PRODUCER; ++i) {
                while (!queue.try_push(i)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (int c = 0; c < PRODUCERS; ++c) {
        threads.emplace_back([&] {
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (queue.try_pop().has_value()) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(consumed.load(), total);
}

// ─── Stress 3: Single-element queue (max contention) ────────────

TEST(StressQueue, LockFreeMaxContention) {
    ate::LockFreeQueue<int> queue(2);  // minimal capacity
    constexpr int OPS = 100'000;
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < OPS; ++i) {
            while (!queue.try_push(i))
                std::this_thread::yield();
        }
    });

    std::thread consumer([&] {
        while (consumed.load(std::memory_order_relaxed) < OPS) {
            if (queue.try_pop().has_value())
                consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(consumed.load(), OPS);
}

} // namespace
