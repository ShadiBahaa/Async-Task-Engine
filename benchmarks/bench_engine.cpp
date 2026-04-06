/// @file bench_engine.cpp
/// @brief Comprehensive benchmarks for the async task engine.
///
/// ═══════════════════════════════════════════════════════════════════
///  BENCHMARKS
/// ═══════════════════════════════════════════════════════════════════
///
/// 1. Thread Pool Throughput     — tasks/sec at varying thread counts
/// 2. Queue Scaling              — mutex vs lock-free at 1/2/4/8 threads
/// 3. Coroutine Overhead         — pool.submit() vs CoroTask vs raw call
/// 4. Asio Timer Latency         — actual vs requested delay
/// 5. AsyncResult Chain Overhead — .then() chain depth cost

#include <ate/asio_context.hpp>
#include <ate/coro_task.hpp>
#include <ate/lockfree_queue.hpp>
#include <ate/task_queue.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <future>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// ─── Helpers ────────────────────────────────────────────────────────

static double to_ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

static void print_header(const char* name) {
    std::printf("\n══════════════════════════════════════════════════\n");
    std::printf("  %s\n", name);
    std::printf("══════════════════════════════════════════════════\n");
}

static void print_row(const char* label, double value, const char* unit) {
    std::printf("  %-30s %12.2f %s\n", label, value, unit);
}

// ═══════════════════════════════════════════════════════════════════
//  1. THREAD POOL THROUGHPUT
// ═══════════════════════════════════════════════════════════════════

static void bench_pool_throughput() {
    print_header("Thread Pool Throughput");
    constexpr int TASKS = 500'000;

    for (int nthreads : {1, 2, 4, 8}) {
        ate::ThreadPool pool(static_cast<std::size_t>(nthreads));
        std::atomic<int> counter{0};

        // Warm up
        for (int i = 0; i < 1000; ++i) {
            pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        while (counter.load(std::memory_order_relaxed) < 1000) {
            std::this_thread::yield();
        }
        counter.store(0, std::memory_order_relaxed);

        auto start = Clock::now();
        for (int i = 0; i < TASKS; ++i) {
            pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        // Wait for all tasks to complete
        while (counter.load(std::memory_order_relaxed) < TASKS) {
            std::this_thread::yield();
        }
        auto elapsed = Clock::now() - start;

        double ms = to_ms(elapsed);
        double ops_sec = static_cast<double>(TASKS) / (ms / 1000.0);

        char label[64];
        std::snprintf(label, sizeof(label), "%d threads (%dK tasks)", nthreads, TASKS / 1000);
        print_row(label, ops_sec / 1e6, "M ops/sec");
    }
}

// ═══════════════════════════════════════════════════════════════════
//  2. QUEUE SCALING — Mutex vs Lock-Free
// ═══════════════════════════════════════════════════════════════════

/// Benchmark the lock-free queue (int-based, try_push/try_pop returning optional).
static double bench_lf_queue(ate::LockFreeQueue<int>& q, int producers,
                              int consumers, int ops_per_producer) {
    std::atomic<int> consumed{0};
    std::atomic<bool> go{false};
    int total_ops = producers * ops_per_producer;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers + consumers));

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, ops_per_producer] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < ops_per_producer; ++i) {
                while (!q.try_push(i))
                    std::this_thread::yield();
            }
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&, total_ops] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (consumed.load(std::memory_order_relaxed) < total_ops) {
                if (q.try_pop().has_value())
                    consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    return to_ms(Clock::now() - start);
}

/// Benchmark the mutex queue (uses move_only_function<void()>).
/// We wrap an int counter bump in a lambda for each item.
static double bench_mutex_queue(ate::TaskQueue& q, int producers,
                                 int consumers, int ops_per_producer) {
    std::atomic<int> consumed{0};
    std::atomic<bool> go{false};
    int total_ops = producers * ops_per_producer;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers + consumers));

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, ops_per_producer] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < ops_per_producer; ++i) {
                q.push([] {});  // push a no-op task
            }
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&, total_ops] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (consumed.load(std::memory_order_relaxed) < total_ops) {
                if (q.try_pop().has_value())
                    consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    return to_ms(Clock::now() - start);
}

static void bench_queue_scaling() {
    print_header("Queue Scaling: Mutex vs Lock-Free");
    constexpr int OPS_PER_PRODUCER = 250'000;

    std::printf("  %-20s %12s %12s %12s\n", "Config", "Mutex (ms)", "LF (ms)", "Speedup");
    std::printf("  %-20s %12s %12s %12s\n", "------", "----------", "-------", "-------");

    for (int n : {1, 2, 4}) {
        ate::TaskQueue mutex_q;
        ate::LockFreeQueue<int> lf_q(1024 * 64);

        double mutex_ms = bench_mutex_queue(mutex_q, n, n, OPS_PER_PRODUCER);
        double lf_ms = bench_lf_queue(lf_q, n, n, OPS_PER_PRODUCER);
        double speedup = mutex_ms / lf_ms;

        char label[32];
        std::snprintf(label, sizeof(label), "%dP/%dC (%dK ops)", n, n,
                      n * OPS_PER_PRODUCER / 1000);
        std::printf("  %-20s %12.1f %12.1f %11.2fx\n", label, mutex_ms, lf_ms, speedup);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  3. COROUTINE OVERHEAD
// ═══════════════════════════════════════════════════════════════════

static ate::CoroTask<int> trivial_coro(ate::ThreadPool& pool) {
    co_await pool.schedule();
    co_return 42;
}

static void bench_coroutine_overhead() {
    print_header("Coroutine Overhead: submit() vs CoroTask");
    constexpr int ITERS = 100'000;

    ate::ThreadPool pool(4);

    // --- submit() baseline ---
    auto start = Clock::now();
    for (int i = 0; i < ITERS; ++i) {
        auto fut = pool.submit([] { return 42; });
        fut.get();
    }
    double submit_ms = to_ms(Clock::now() - start);

    // --- CoroTask ---
    start = Clock::now();
    for (int i = 0; i < ITERS; ++i) {
        auto task = trivial_coro(pool);
        [[maybe_unused]] auto result = task.get();
    }
    double coro_ms = to_ms(Clock::now() - start);

    print_row("submit() + fut.get()", submit_ms, "ms total");
    print_row("CoroTask + get()", coro_ms, "ms total");
    print_row("submit() per-task", submit_ms / ITERS * 1000.0, "µs");
    print_row("CoroTask per-task", coro_ms / ITERS * 1000.0, "µs");
    print_row("Coro overhead", (coro_ms / submit_ms - 1.0) * 100.0, "%");
}

// ═══════════════════════════════════════════════════════════════════
//  4. ASIO TIMER LATENCY
// ═══════════════════════════════════════════════════════════════════

static void bench_asio_latency() {
    print_header("Asio Timer Latency");
    constexpr int ITERS = 100;

    ate::ThreadPool pool(4);
    ate::AsioContext asio(pool, 2);

    std::vector<double> latencies;
    latencies.reserve(ITERS);

    for (int i = 0; i < ITERS; ++i) {
        auto start = Clock::now();
        auto fut = asio.async_after(1ms, [] {});
        fut.get();
        double actual_ms = to_ms(Clock::now() - start);
        latencies.push_back(actual_ms - 1.0);  // overshoot in ms
    }

    // Sort for percentile stats
    std::sort(latencies.begin(), latencies.end());

    double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0)
                  / static_cast<double>(latencies.size());
    double p50 = latencies[static_cast<std::size_t>(ITERS * 0.50)];
    double p95 = latencies[static_cast<std::size_t>(ITERS * 0.95)];
    double p99 = latencies[static_cast<std::size_t>(ITERS * 0.99)];

    print_row("Mean overshoot", mean, "ms");
    print_row("P50 overshoot", p50, "ms");
    print_row("P95 overshoot", p95, "ms");
    print_row("P99 overshoot", p99, "ms");
}

// ═══════════════════════════════════════════════════════════════════
//  5. ASYNC RESULT CHAIN OVERHEAD
// ═══════════════════════════════════════════════════════════════════

static void bench_async_chain() {
    print_header("AsyncResult .then() Chain Depth");
    constexpr int ITERS = 10'000;

    ate::ThreadPool pool(4);

    for (int depth : {1, 5, 10}) {
        auto start = Clock::now();

        for (int i = 0; i < ITERS; ++i) {
            auto ar = pool.submit_async([] { return 1; });
            for (int d = 0; d < depth; ++d) {
                ar = ar.then([](int v) { return v + 1; });
            }
            [[maybe_unused]] auto result = ar.get();
        }

        double ms = to_ms(Clock::now() - start);
        char label[64];
        std::snprintf(label, sizeof(label), "depth=%d (%dK chains)", depth, ITERS / 1000);
        print_row(label, ms / ITERS * 1000.0, "µs/chain");
    }
}

// ═══════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════

int main() {
    std::printf("Async Task Engine — Benchmark Suite\n");
    std::printf("Threads available: %u\n",
                std::thread::hardware_concurrency());

    bench_pool_throughput();
    bench_queue_scaling();
    bench_coroutine_overhead();
    bench_asio_latency();
    bench_async_chain();

    std::printf("\n══════════════════════════════════════════════════\n");
    std::printf("  All benchmarks complete.\n");
    std::printf("══════════════════════════════════════════════════\n\n");

    return 0;
}
