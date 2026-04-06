cmake_minimum_required(VERSION 3.22)
project(async_task_engine
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "High-performance asynchronous task processing engine"
)

# ─── C++ Standard ────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Compiler Warnings (treat warnings as errors in CI) ─────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align
    -Woverloaded-virtual
)

# ─── Threading ───────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─── Boost (Asio) ────────────────────────────────────────────────────
find_package(Boost REQUIRED COMPONENTS system)

# ─── Core Library ────────────────────────────────────────────────────
add_library(ate
    src/thread_pool.cpp
)
target_include_directories(ate PUBLIC include)
target_link_libraries(ate PUBLIC Threads::Threads Boost::system)

# ─── Google Test ─────────────────────────────────────────────────────
find_package(GTest REQUIRED)
enable_testing()

# ─── Unit Tests ──────────────────────────────────────────────────────
add_executable(test_task_queue   tests/unit/test_task_queue.cpp)
target_link_libraries(test_task_queue PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME TaskQueueTests COMMAND test_task_queue)

add_executable(test_thread_pool  tests/unit/test_thread_pool.cpp)
target_link_libraries(test_thread_pool PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME ThreadPoolTests COMMAND test_thread_pool)

add_executable(test_async_result tests/unit/test_async_result.cpp)
target_link_libraries(test_async_result PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME AsyncResultTests COMMAND test_async_result)

add_executable(test_lockfree_queue tests/unit/test_lockfree_queue.cpp)
target_link_libraries(test_lockfree_queue PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME LockFreeQueueTests COMMAND test_lockfree_queue)

add_executable(test_coro_task tests/unit/test_coro_task.cpp)
target_link_libraries(test_coro_task PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME CoroTaskTests COMMAND test_coro_task)

add_executable(test_asio_context tests/unit/test_asio_context.cpp)
target_link_libraries(test_asio_context PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME AsioContextTests COMMAND test_asio_context)

add_executable(test_logger tests/unit/test_logger.cpp)
target_link_libraries(test_logger PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME LoggerTests COMMAND test_logger)

add_executable(test_scheduler tests/unit/test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME SchedulerTests COMMAND test_scheduler)

add_executable(test_pipeline tests/unit/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME PipelineTests COMMAND test_pipeline)

# ─── Stress Tests ────────────────────────────────────────────────────
add_executable(stress_pool tests/stress/stress_pool.cpp)
target_link_libraries(stress_pool PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME StressPoolTests COMMAND stress_pool)

add_executable(stress_queue tests/stress/stress_queue.cpp)
target_link_libraries(stress_queue PRIVATE ate GTest::GTest GTest::Main)
add_test(NAME StressQueueTests COMMAND stress_queue)

# ─── Benchmarks ──────────────────────────────────────────────────────
add_executable(bench_queue benchmarks/bench_queue.cpp)
target_include_directories(bench_queue PRIVATE include)
target_link_libraries(bench_queue PRIVATE Threads::Threads)

add_executable(bench_engine benchmarks/bench_engine.cpp)
target_link_libraries(bench_engine PRIVATE ate)

