# ⚡ Async Task Engine (ATE)

A production-grade, high-performance asynchronous task processing engine in **C++23**.

Built with lock-free data structures, C++20 coroutines, Boost.Asio integration, and comprehensive sanitizer-verified concurrency — from the ground up.

---

## Features

| Component | Description |
|---|---|
| **ThreadPool** | N `std::jthread` workers with cooperative `stop_token` shutdown |
| **TaskQueue** | Thread-safe MPMC queue (mutex + condition variable) |
| **LockFreeQueue** | Vyukov bounded MPMC ring buffer — **up to 8.9× faster** than mutex |
| **AsyncResult\<T\>** | Future/promise API with `.then()` continuation chaining |
| **CoroTask\<T\>** | Lazy C++20 coroutine with `co_await pool.schedule()` |
| **AsioContext** | Boost.Asio `io_context` on dedicated threads — timers, async I/O |
| **Pipeline\<T\>** | Type-safe composable stages: `create(pool, x).then(f1).then(f2).run()` |
| **PriorityScheduler** | 4-level priority queue (Low/Normal/High/Critical) with FIFO ordering |
| **Logger** | Async drain-thread logger with runtime level control and custom sinks |

---

## Quick Start

```cpp
#include <ate/thread_pool.hpp>
#include <ate/async_result.hpp>
#include <ate/coro_task.hpp>
#include <ate/pipeline.hpp>

int main() {
    ate::ThreadPool pool(4);

    // 1. Simple submit
    auto future = pool.submit([] { return 42; });
    int result = future.get();  // 42

    // 2. Chained continuations
    auto chained = pool.submit_async([] { return 10; })
        .then([](int x) { return x * 2; })
        .then([](int x) { return x + 5; });
    int val = chained.get();  // 25

    // 3. Pipeline
    auto pipe = ate::Pipeline<int>::create(pool, 100)
        .then([](int x) { return x * 3; })
        .then([](int x) { return std::to_string(x); })
        .run();
    std::string s = pipe.get();  // "300"
}
```

### Coroutines

```cpp
#include <ate/coro_task.hpp>

ate::CoroTask<int> compute(ate::ThreadPool& pool) {
    co_await pool.schedule();      // offload to pool thread
    int a = expensive_compute();
    co_return a * 2;
}

auto task = compute(pool);
int result = task.get();
```

### Async I/O (Boost.Asio)

```cpp
#include <ate/asio_context.hpp>

ate::ThreadPool pool(4);
ate::AsioContext asio(pool, 2);  // 2 I/O threads

// Timer-based delayed execution
auto fut = asio.async_after(100ms, [] { return "done"; });
std::string s = fut.get();

// Coroutine delay
ate::CoroTask<void> delayed(ate::AsioContext& asio) {
    co_await asio.delay(50ms);
    // ... runs after 50ms, non-blocking
}
```

---

## Build

### Requirements

| Dependency | Minimum Version |
|---|---|
| C++ Compiler | GCC 13+ (C++23). Clang not yet supported (libc++ lacks `jthread`/`stop_token`) |
| CMake | 3.22 |
| Boost | 1.74 (system) |
| GoogleTest | 1.14 |

### Linux / WSL

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install g++-13 cmake ninja-build libboost-all-dev libgtest-dev

# Build
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run benchmarks
./build/bench_engine
```

### Sanitizer Builds

```bash
# ThreadSanitizer
cmake -B build-tsan -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan && ctest --test-dir build-tsan

# AddressSanitizer
cmake -B build-asan -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan && ctest --test-dir build-asan
```

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                         User Code                            │
│  pool.submit(fn)   CoroTask   Pipeline   pool.submit_async  │
└──────┬───────────────┬───────────┬──────────────┬────────────┘
       │               │           │              │
       ▼               ▼           ▼              ▼
┌─────────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐
│ std::future │ │CoroTask  │ │ Pipeline │ │ AsyncResult  │
│             │ │(lazy)    │ │ (chain)  │ │ (.then())    │
└──────┬──────┘ └─────┬────┘ └────┬─────┘ └──────┬───────┘
       └──────────┬───┘───────────┘───────────────┘
                  ▼
          ┌───────────────┐
          │   TaskQueue   │     FIFO (mutex) or PriorityScheduler
          │      or       │
          │ LockFreeQueue │     Vyukov MPMC (lock-free)
          └──────┬────────┘
                 ▼
          ┌───────────────┐
          │  ThreadPool   │     N jthread workers
          │  ┌─┐┌─┐┌─┐   │
          │  │W││W││W│…   │
          │  └─┘└─┘└─┘   │
          └──────┬────────┘
       ┌─────────┴─────────┐
       ▼                   ▼
┌─────────────┐    ┌──────────────┐
│   Logger    │    │ AsioContext  │
│ (async log) │    │ (io_context) │
└─────────────┘    └──────────────┘
```

---

## Project Structure

```
Project/
├── CMakeLists.txt
├── .clang-tidy
├── .github/workflows/ci.yml
├── include/ate/
│   ├── thread_pool.hpp        # Thread pool with jthread workers
│   ├── task_queue.hpp         # Mutex-based MPMC queue
│   ├── task.hpp               # Type-erased callable wrapper
│   ├── lockfree_queue.hpp     # Vyukov bounded lock-free MPMC
│   ├── async_result.hpp       # Future wrapper with .then()
│   ├── coro_task.hpp          # C++20 lazy coroutine
│   ├── asio_context.hpp       # Boost.Asio integration
│   ├── pipeline.hpp           # Composable async pipeline
│   ├── scheduler.hpp          # Priority scheduler
│   └── logger.hpp             # Async drain-thread logger
├── src/
│   └── thread_pool.cpp
├── tests/
│   ├── unit/                  # 9 unit test suites
│   └── stress/                # 2 stress test suites
└── benchmarks/
    ├── bench_queue.cpp        # Queue-only microbenchmark
    └── bench_engine.cpp       # Full engine benchmark suite
```

---

## Benchmarks

*Release build, GCC 13, measured on WSL.*

### Thread Pool Throughput

| Threads | Throughput |
|---|---|
| 1 | 1.39 M ops/sec |
| 2 | 1.06 M ops/sec |
| 4 | 0.34 M ops/sec |

### Lock-Free vs Mutex Queue

| Contention | Mutex | Lock-Free | Speedup |
|---|---|---|---|
| 1P/1C | 43.6 ms | 4.9 ms | **8.89×** |
| 2P/2C | 118.8 ms | 42.5 ms | **2.79×** |
| 4P/4C | 363.7 ms | 146.1 ms | **2.49×** |

### Coroutine Overhead

| Method | Per-task |
|---|---|
| `submit() + get()` | 35.8 µs |
| `CoroTask + get()` | 38.8 µs (+8.4%) |

### Asio Timer Latency (1ms target)

| Percentile | Overshoot |
|---|---|
| P50 | 0.20 ms |
| P99 | 0.54 ms |

---

## CI/CD

GitHub Actions pipeline with 5 parallel jobs:

| Job | Details |
|---|---|
| **build** | GCC-13 × Debug + Release |
| **sanitizers** | TSan, ASan, UBSan (parallel, `halt_on_error=1`) |
| **stress** | Heavy concurrency tests, 300s timeout |
| **benchmark** | Release build + regression gate (fails if speedup < 1.5×) |
| **static-analysis** | clang-tidy with `--warnings-as-errors` |

---

## Key Design Decisions

| Decision | Choice | Why |
|---|---|---|
| Thread model | `std::jthread` | Auto-join + `stop_token` = no leaked threads |
| Task type-erasure | `std::move_only_function` (C++23) | Supports move-only lambdas, no copy overhead |
| Lock-free queue | Vyukov bounded MPMC | Per-slot sequence counters, no syscalls |
| Coroutine signaling | `FinalAwaiter::await_suspend()` | Prevents use-after-free (signal after suspension) |
| False sharing | `alignas(64)` on hot atomics | Separate cache lines for enqueue/dequeue positions |
| Asio integration | Dedicated I/O threads | Decouples CPU-bound from I/O-bound work |

---

## Testing

```
11 test suites, all sanitizer-clean:

 ✅ TaskQueueTests        ✅ LoggerTests
 ✅ ThreadPoolTests       ✅ SchedulerTests
 ✅ AsyncResultTests      ✅ PipelineTests
 ✅ LockFreeQueueTests    ✅ StressPoolTests
 ✅ CoroTaskTests         ✅ StressQueueTests
 ✅ AsioContextTests
```

| Sanitizer | Status |
|---|---|
| ThreadSanitizer | ✅ Zero data races |
| AddressSanitizer | ✅ Zero memory errors |
| UndefinedBehaviorSanitizer | ✅ Zero UB |

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
