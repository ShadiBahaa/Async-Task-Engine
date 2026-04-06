/// @file test_pipeline.cpp
/// @brief Tests for ate::Pipeline.

#include <ate/pipeline.hpp>
#include <gtest/gtest.h>

#include <string>

namespace {

TEST(PipelineTest, SingleStage) {
    ate::ThreadPool pool(4);

    auto fut = ate::Pipeline<int>::create(pool, 10)
        .then([](int x) { return x * 2; })
        .run();

    EXPECT_EQ(fut.get(), 20);
}

TEST(PipelineTest, MultipleStages) {
    ate::ThreadPool pool(4);

    auto fut = ate::Pipeline<int>::create(pool, 5)
        .then([](int x) { return x * 2; })      // 10
        .then([](int x) { return x + 3; })       // 13
        .then([](int x) { return x * x; })       // 169
        .run();

    EXPECT_EQ(fut.get(), 169);
}

TEST(PipelineTest, TypeChangingStages) {
    ate::ThreadPool pool(4);

    auto fut = ate::Pipeline<int>::create(pool, 42)
        .then([](int x) { return x * 2; })                        // int → int
        .then([](int x) { return std::to_string(x); })            // int → string
        .then([](std::string s) { return s + " is the answer"; }) // string → string
        .run();

    EXPECT_EQ(fut.get(), "84 is the answer");
}

TEST(PipelineTest, NoStages) {
    ate::ThreadPool pool(4);

    auto fut = ate::Pipeline<int>::create(pool, 99).run();
    EXPECT_EQ(fut.get(), 99);
}

TEST(PipelineTest, StringPipeline) {
    ate::ThreadPool pool(4);

    auto fut = ate::Pipeline<std::string>::create(pool, "hello")
        .then([](std::string s) { return s + " world"; })
        .then([](std::string s) { return s + "!"; })
        .run();

    EXPECT_EQ(fut.get(), "hello world!");
}

TEST(PipelineTest, MultiplePipelines) {
    ate::ThreadPool pool(4);

    auto f1 = ate::Pipeline<int>::create(pool, 1)
        .then([](int x) { return x + 1; })
        .run();

    auto f2 = ate::Pipeline<int>::create(pool, 100)
        .then([](int x) { return x - 1; })
        .run();

    EXPECT_EQ(f1.get(), 2);
    EXPECT_EQ(f2.get(), 99);
}

TEST(PipelineTest, ExceptionPropagation) {
    ate::ThreadPool pool(4);

    auto fut = ate::Pipeline<int>::create(pool, 0)
        .then([](int) -> int { throw std::runtime_error("pipeline error"); })
        .run();

    EXPECT_THROW(fut.get(), std::runtime_error);
}

} // namespace
