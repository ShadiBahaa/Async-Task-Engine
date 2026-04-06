/// @file bench_queue.cpp
/// @brief Microbenchmark: mutex queue vs lock-free queue.
///
/// What this measures
/// ──────────────────
/// We compare two implementations of a task queue under identical
/// workloads:
///   1. ate::TaskQueue      — mutex + condition_variable
///   2. ate::LockFreeQueue  — Vyukov bounded MPMC
///
/// Workload: N producers each push M integers, N consumers pop them.
/// We measure total wall-clock time (throughput = total_ops / time).
///
/// This is NOT a Google Benchmark file (to avoid the dependency for now).
/// It's a standalone executable that prints results to stdout.
/// We can upgrade to Google Benchmark in Phase 7.
///
/// Run: ./bench_queue
/// Output: throughput numbers for both implementations.

#include <ate/lockfree_queue.hpp>
#include <ate/task_queue.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

// ─── Configuration ──────────────────────────────────────────────────

constexpr int NUM_PRODUCERS   = 4;
constexpr int NUM_CONSUMERS   = 4;
constexpr int ITEMS_EACH      = 500'000;
constexpr int TOTAL_ITEMS     = NUM_PRODUCERS * ITEMS_EACH;

// ─── Benchmark: Mutex-based TaskQueue ───────────────────────────────

double bench_mutex_queue() {
    ate::TaskQueue q;
    std::atomic<int> consumed{0};

    auto start = std::chrono::high_resolution_clock::now();

    // Producers: push trivial callables
    std::vector<std::jthread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&q] {
            for (int i = 0; i < ITEMS_EACH; ++i) {
                while (!q.push([]{} )) {
                    // Queue shut down (shouldn't happen in benchmark)
                }
            }
        });
    }

    // Consumers: pop and count
    std::vector<std::jthread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&q, &consumed] {
            while (consumed.load(std::memory_order_relaxed) < TOTAL_ITEMS) {
                if (auto task = q.try_pop()) {
                    (*task)();  // execute the no-op
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    producers.clear();  // join

    // Spin until all consumed
    while (consumed.load(std::memory_order_relaxed) < TOTAL_ITEMS) {
        std::this_thread::yield();
    }

    q.shutdown();
    consumers.clear();  // join

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    return ms;
}

// ─── Benchmark: Lock-free queue ─────────────────────────────────────

double bench_lockfree_queue() {
    ate::LockFreeQueue<int> q(65536);  // 64K slots
    std::atomic<int> consumed{0};

    auto start = std::chrono::high_resolution_clock::now();

    // Producers
    std::vector<std::jthread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&q, p] {
            int base = p * ITEMS_EACH;
            for (int i = 0; i < ITEMS_EACH; ++i) {
                while (!q.try_push(base + i)) {
                    // Spin — full
                }
            }
        });
    }

    // Consumers
    std::vector<std::jthread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&q, &consumed] {
            while (consumed.load(std::memory_order_relaxed) < TOTAL_ITEMS) {
                if (auto val = q.try_pop()) {
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    producers.clear();

    while (consumed.load(std::memory_order_relaxed) < TOTAL_ITEMS) {
        std::this_thread::yield();
    }

    consumers.clear();

    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    return ms;
}

// ─── Main ───────────────────────────────────────────────────────────

int main() {
    std::printf("═══════════════════════════════════════════════════\n");
    std::printf("  Queue Benchmark: Mutex vs Lock-Free\n");
    std::printf("  Producers: %d  Consumers: %d  Items/producer: %d\n",
                NUM_PRODUCERS, NUM_CONSUMERS, ITEMS_EACH);
    std::printf("  Total operations: %d\n", TOTAL_ITEMS);
    std::printf("═══════════════════════════════════════════════════\n\n");

    // Warm up
    bench_mutex_queue();
    bench_lockfree_queue();

    // Actual run (average 3 iterations)
    constexpr int RUNS = 3;

    double mutex_total = 0;
    double lockfree_total = 0;

    for (int r = 0; r < RUNS; ++r) {
        mutex_total    += bench_mutex_queue();
        lockfree_total += bench_lockfree_queue();
    }

    double mutex_avg    = mutex_total / RUNS;
    double lockfree_avg = lockfree_total / RUNS;

    double mutex_throughput    = TOTAL_ITEMS / (mutex_avg / 1000.0);
    double lockfree_throughput = TOTAL_ITEMS / (lockfree_avg / 1000.0);

    std::printf("  Mutex Queue:     %8.2f ms  (%12.0f ops/sec)\n",
                mutex_avg, mutex_throughput);
    std::printf("  Lock-Free Queue: %8.2f ms  (%12.0f ops/sec)\n",
                lockfree_avg, lockfree_throughput);
    std::printf("\n  Speedup: %.2fx\n",
                mutex_avg / lockfree_avg);
    std::printf("═══════════════════════════════════════════════════\n");

    return 0;
}
