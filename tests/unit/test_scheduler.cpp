/// @file test_scheduler.cpp
/// @brief Tests for ate::PriorityScheduler.

#include <ate/scheduler.hpp>
#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace {

TEST(SchedulerTest, PushAndPop) {
    ate::PriorityScheduler sched;

    sched.push([] {}, ate::TaskPriority::Normal);
    auto task = sched.try_pop();
    ASSERT_TRUE(task.has_value());
    task->func();  // should not throw
}

TEST(SchedulerTest, PopEmptyReturnsNullopt) {
    ate::PriorityScheduler sched;
    EXPECT_FALSE(sched.try_pop().has_value());
}

TEST(SchedulerTest, HighPriorityFirst) {
    ate::PriorityScheduler sched;

    int order = 0;
    int low_order = -1, high_order = -1, crit_order = -1;

    sched.push([&] { low_order  = order++; }, ate::TaskPriority::Low);
    sched.push([&] { high_order = order++; }, ate::TaskPriority::High);
    sched.push([&] { crit_order = order++; }, ate::TaskPriority::Critical);

    // Pop should return Critical, then High, then Low
    auto t1 = sched.try_pop(); t1->func();
    auto t2 = sched.try_pop(); t2->func();
    auto t3 = sched.try_pop(); t3->func();

    EXPECT_EQ(crit_order, 0);
    EXPECT_EQ(high_order, 1);
    EXPECT_EQ(low_order, 2);
}

TEST(SchedulerTest, FIFOWithinSamePriority) {
    ate::PriorityScheduler sched;

    std::vector<int> execution_order;

    for (int i = 0; i < 5; ++i) {
        sched.push([&, i] { execution_order.push_back(i); },
                   ate::TaskPriority::Normal);
    }

    for (int i = 0; i < 5; ++i) {
        auto t = sched.try_pop();
        ASSERT_TRUE(t.has_value());
        t->func();
    }

    EXPECT_EQ(execution_order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(SchedulerTest, ShutdownUnblocks) {
    ate::PriorityScheduler sched;

    std::jthread worker([&](std::stop_token st) {
        auto task = sched.wait_and_pop(st);
        // Should return nullopt after shutdown
        EXPECT_FALSE(task.has_value());
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sched.shutdown();
}

TEST(SchedulerTest, PushAfterShutdown) {
    ate::PriorityScheduler sched;
    sched.shutdown();
    EXPECT_FALSE(sched.push([] {}, ate::TaskPriority::Normal));
}

TEST(SchedulerTest, Size) {
    ate::PriorityScheduler sched;

    EXPECT_EQ(sched.size(), 0u);
    EXPECT_TRUE(sched.empty());

    sched.push([] {}, ate::TaskPriority::Normal);
    sched.push([] {}, ate::TaskPriority::High);

    EXPECT_EQ(sched.size(), 2u);
    EXPECT_FALSE(sched.empty());
}

TEST(SchedulerTest, ConcurrentPushPop) {
    ate::PriorityScheduler sched;
    constexpr int N = 10'000;
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            sched.push([] {}, ate::TaskPriority::Normal);
        }
    });

    std::thread consumer([&] {
        while (consumed.load() < N) {
            if (auto t = sched.try_pop()) {
                t->func();
                consumed.fetch_add(1);
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(consumed.load(), N);
}

} // namespace
