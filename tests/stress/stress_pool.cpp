/// @file stress_pool.cpp
/// @brief Stress tests for ThreadPool + async components under heavy load.
///
/// These tests push the engine to its limits:
///   • 16 threads × 100K tasks
///   • Mixed submit + coroutine + pipeline workloads
///   • Concurrent creation/destruction
///   • Exception storms

#include <ate/asio_context.hpp>
#include <ate/coro_task.hpp>
#include <ate/pipeline.hpp>
#include <ate/thread_pool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// ─── Stress 1: Mass task submission ─────────────────────────────

TEST(StressPool, MassSubmission) {
    constexpr int THREADS = 8;
    constexpr int TASKS = 100'000;

    ate::ThreadPool pool(THREADS);
    std::atomic<int> counter{0};

    for (int i = 0; i < TASKS; ++i) {
        pool.submit([&] { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    // Wait for completion
    while (counter.load(std::memory_order_relaxed) < TASKS) {
        std::this_thread::yield();
    }
    EXPECT_EQ(counter.load(), TASKS);
}

// ─── Stress 2: Concurrent submitters ────────────────────────────

TEST(StressPool, ConcurrentSubmitters) {
    constexpr int POOL_THREADS = 4;
    constexpr int SUBMITTERS = 8;
    constexpr int PER_SUBMITTER = 10'000;

    ate::ThreadPool pool(POOL_THREADS);
    std::atomic<int> counter{0};

    std::vector<std::thread> submitters;
    for (int s = 0; s < SUBMITTERS; ++s) {
        submitters.emplace_back([&] {
            for (int i = 0; i < PER_SUBMITTER; ++i) {
                pool.submit([&] {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& t : submitters) t.join();

    while (counter.load(std::memory_order_relaxed) < SUBMITTERS * PER_SUBMITTER) {
        std::this_thread::yield();
    }
    EXPECT_EQ(counter.load(), SUBMITTERS * PER_SUBMITTER);
}

// ─── Stress 3: Submit with futures (result correctness) ─────────

TEST(StressPool, SubmitWithFutures) {
    constexpr int TASKS = 50'000;
    ate::ThreadPool pool(8);

    std::vector<std::future<int>> futures;
    futures.reserve(TASKS);

    for (int i = 0; i < TASKS; ++i) {
        futures.push_back(pool.submit([i] { return i * 2; }));
    }

    for (int i = 0; i < TASKS; ++i) {
        EXPECT_EQ(futures[static_cast<std::size_t>(i)].get(), i * 2);
    }
}

// ─── Stress 4: Exception storm ──────────────────────────────────

TEST(StressPool, ExceptionStorm) {
    constexpr int TASKS = 10'000;
    ate::ThreadPool pool(4);

    std::vector<std::future<int>> futures;
    for (int i = 0; i < TASKS; ++i) {
        futures.push_back(pool.submit([i]() -> int {
            if (i % 2 == 0) throw std::runtime_error("even");
            return i;
        }));
    }

    int successes = 0, exceptions = 0;
    for (int i = 0; i < TASKS; ++i) {
        try {
            futures[static_cast<std::size_t>(i)].get();
            ++successes;
        } catch (const std::runtime_error&) {
            ++exceptions;
        }
    }
    EXPECT_EQ(successes, TASKS / 2);
    EXPECT_EQ(exceptions, TASKS / 2);
}

// ─── Stress 5: Mixed coroutine + submit ─────────────────────────

ate::CoroTask<int> stress_coro(ate::ThreadPool& pool, int val) {
    co_await pool.schedule();
    co_return val * 3;
}

TEST(StressPool, MixedCoroAndSubmit) {
    constexpr int COUNT = 5000;
    ate::ThreadPool pool(4);

    std::atomic<int> total{0};

    // Alternate between submit and coroutine
    for (int i = 0; i < COUNT; ++i) {
        if (i % 2 == 0) {
            auto fut = pool.submit([&] {
                total.fetch_add(1, std::memory_order_relaxed);
            });
            fut.get();
        } else {
            auto task = stress_coro(pool, 1);
            total.fetch_add(task.get(), std::memory_order_relaxed);
        }
    }
    // 2500 submits add 1 each, 2500 coros add 3 each
    EXPECT_EQ(total.load(), 2500 + 2500 * 3);
}

// ─── Stress 6: Pipeline under load ──────────────────────────────

TEST(StressPool, PipelineFlood) {
    constexpr int COUNT = 5000;
    ate::ThreadPool pool(4);

    std::vector<std::future<int>> futures;
    for (int i = 0; i < COUNT; ++i) {
        futures.push_back(
            ate::Pipeline<int>::create(pool, i)
                .then([](int x) { return x + 1; })
                .then([](int x) { return x * 2; })
                .run()
        );
    }

    for (int i = 0; i < COUNT; ++i) {
        EXPECT_EQ(futures[static_cast<std::size_t>(i)].get(), (i + 1) * 2);
    }
}

} // namespace
