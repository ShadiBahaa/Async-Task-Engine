/// @file test_logger.cpp
/// @brief Tests for ate::Logger.

#include <ate/logger.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

TEST(LoggerTest, BasicLogging) {
    std::vector<std::string> captured;
    std::mutex mu;

    ate::Logger logger(ate::LogLevel::Info,
        [&](const ate::LogEntry& e) {
            std::lock_guard lock(mu);
            captured.push_back(e.message);
        });

    logger.info("hello");
    logger.warn("world");
    logger.flush();

    ASSERT_EQ(captured.size(), 2u);
    EXPECT_EQ(captured[0], "hello");
    EXPECT_EQ(captured[1], "world");
}

TEST(LoggerTest, LevelFiltering) {
    std::atomic<int> count{0};

    ate::Logger logger(ate::LogLevel::Warn,
        [&](const ate::LogEntry&) { count.fetch_add(1); });

    logger.trace("filtered");
    logger.debug("filtered");
    logger.info("filtered");
    logger.warn("kept");
    logger.error("kept");
    logger.flush();

    EXPECT_EQ(count.load(), 2);
}

TEST(LoggerTest, RuntimeLevelChange) {
    std::atomic<int> count{0};

    ate::Logger logger(ate::LogLevel::Error,
        [&](const ate::LogEntry&) { count.fetch_add(1); });

    logger.info("filtered");
    logger.flush();
    EXPECT_EQ(count.load(), 0);

    logger.set_level(ate::LogLevel::Info);
    logger.info("now kept");
    logger.flush();
    EXPECT_EQ(count.load(), 1);
}

TEST(LoggerTest, MultiThreadedLogging) {
    std::atomic<int> count{0};

    ate::Logger logger(ate::LogLevel::Trace,
        [&](const ate::LogEntry&) { count.fetch_add(1); });

    constexpr int THREADS = 8;
    constexpr int PER_THREAD = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < PER_THREAD; ++i) {
                logger.info("msg from thread " + std::to_string(t));
            }
        });
    }
    for (auto& thr : threads) thr.join();
    logger.flush();

    EXPECT_EQ(count.load(), THREADS * PER_THREAD);
}

TEST(LoggerTest, EntryHasCorrectLevel) {
    ate::LogLevel captured_level{};

    ate::Logger logger(ate::LogLevel::Trace,
        [&](const ate::LogEntry& e) { captured_level = e.level; });

    logger.error("test");
    logger.flush();

    EXPECT_EQ(captured_level, ate::LogLevel::Error);
}

TEST(LoggerTest, FlushBlocksUntilDrained) {
    std::atomic<int> count{0};

    ate::Logger logger(ate::LogLevel::Trace,
        [&](const ate::LogEntry&) {
            std::this_thread::sleep_for(1ms);
            count.fetch_add(1);
        });

    for (int i = 0; i < 20; ++i) {
        logger.info("msg");
    }
    logger.flush();

    // After flush, ALL messages must have been processed
    EXPECT_EQ(count.load(), 20);
}

} // namespace
