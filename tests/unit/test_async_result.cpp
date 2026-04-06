/// @file test_async_result.cpp
/// @brief Unit tests for ate::AsyncResult<T> and .then() chaining.
///
/// What we test and WHY
/// ────────────────────
/// 1. Basic submit_async — verifies AsyncResult wraps future correctly.
/// 2. Single .then()     — continuation receives predecessor's result.
/// 3. Multi-stage chain  — a.then(b).then(c) composes correctly.
/// 4. Type-changing chain — int → string across continuations.
/// 5. Void predecessor   — .then() on a void result works.
/// 6. Exception propagation through chain — if stage 1 throws,
///    stage 2 should NOT run, and .get() re-throws the original.
/// 7. Concurrent chains  — many independent chains running in parallel.

#include <ate/thread_pool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════════
// Test 1: Basic submit_async
// ═══════════════════════════════════════════════════════════════════

TEST(AsyncResultTest, BasicSubmitAsync) {
    ate::ThreadPool pool(2);

    auto result = pool.submit_async([] { return 42; });
    EXPECT_EQ(result.get(), 42);
}

// ═══════════════════════════════════════════════════════════════════
// Test 2: Single .then()
// ═══════════════════════════════════════════════════════════════════
// The continuation receives the predecessor's result as its argument.

TEST(AsyncResultTest, SingleThen) {
    ate::ThreadPool pool(2);

    auto result = pool.submit_async([] { return 10; })
                      .then([](int x) { return x * 3; });

    EXPECT_EQ(result.get(), 30);
}

// ═══════════════════════════════════════════════════════════════════
// Test 3: Multi-stage chain
// ═══════════════════════════════════════════════════════════════════
// a.then(b).then(c) → c(b(a()))

TEST(AsyncResultTest, MultiStageChain) {
    ate::ThreadPool pool(4);

    auto result = pool.submit_async([] { return 2; })
                      .then([](int x) { return x + 3; })    // 5
                      .then([](int x) { return x * 4; })    // 20
                      .then([](int x) { return x - 1; });   // 19

    EXPECT_EQ(result.get(), 19);
}

// ═══════════════════════════════════════════════════════════════════
// Test 4: Type-changing chain (int → string)
// ═══════════════════════════════════════════════════════════════════

TEST(AsyncResultTest, TypeChangingChain) {
    ate::ThreadPool pool(2);

    auto result = pool.submit_async([] { return 42; })
                      .then([](int x) { return std::to_string(x); })
                      .then([](const std::string& s) { return "Value: " + s; });

    EXPECT_EQ(result.get(), "Value: 42");
}

// ═══════════════════════════════════════════════════════════════════
// Test 5: Void predecessor .then()
// ═══════════════════════════════════════════════════════════════════
// A void task followed by .then() that takes no arguments.

TEST(AsyncResultTest, VoidThen) {
    ate::ThreadPool pool(2);
    std::atomic<int> side_effect{0};

    auto result = pool.submit_async([&side_effect] {
                          side_effect.store(1);
                      })
                      .then([&side_effect] {
                          return side_effect.load() + 99;
                      });

    EXPECT_EQ(result.get(), 100);
}

// ═══════════════════════════════════════════════════════════════════
// Test 6: Exception propagation through chain
// ═══════════════════════════════════════════════════════════════════
// If stage 1 throws, .then() never executes the continuation.
// The exception propagates to the final .get().

TEST(AsyncResultTest, ExceptionPropagation) {
    ate::ThreadPool pool(2);

    std::atomic<bool> continuation_ran{false};

    auto result = pool.submit_async([]() -> int {
                          throw std::runtime_error("fail");
                      })
                      .then([&continuation_ran](int x) {
                          continuation_ran.store(true);
                          return x * 2;
                      });

    EXPECT_THROW(result.get(), std::runtime_error);
    EXPECT_FALSE(continuation_ran.load());
}

// ═══════════════════════════════════════════════════════════════════
// Test 7: Multiple independent chains concurrently
// ═══════════════════════════════════════════════════════════════════

TEST(AsyncResultTest, ConcurrentChains) {
    ate::ThreadPool pool(4);
    constexpr int num_chains = 100;

    std::vector<ate::AsyncResult<int>> results;
    results.reserve(num_chains);

    for (int i = 0; i < num_chains; ++i) {
        results.push_back(
            pool.submit_async([i] { return i; })
                .then([](int x) { return x * 2; })
                .then([](int x) { return x + 1; })
        );
    }

    for (int i = 0; i < num_chains; ++i) {
        EXPECT_EQ(results[static_cast<std::size_t>(i)].get(), i * 2 + 1);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test 8: submit_async with arguments
// ═══════════════════════════════════════════════════════════════════

TEST(AsyncResultTest, SubmitAsyncWithArgs) {
    ate::ThreadPool pool(2);

    auto result = pool.submit_async([](int a, int b) { return a * b; }, 6, 7)
                      .then([](int x) { return x + 1; });

    EXPECT_EQ(result.get(), 43);
}

// ═══════════════════════════════════════════════════════════════════
// Test 9: Deep chain (test for no stack overflow with pool dispatch)
// ═══════════════════════════════════════════════════════════════════
// Since .then() re-submits to the pool (not inline), a deep chain
// should NOT cause stack overflow.

TEST(AsyncResultTest, DeepChain) {
    ate::ThreadPool pool(4);

    auto result = pool.submit_async([] { return 0; });

    // Chain 50 continuations
    for (int i = 0; i < 50; ++i) {
        result = std::move(result).then([](int x) { return x + 1; });
    }

    EXPECT_EQ(result.get(), 50);
}

} // namespace
