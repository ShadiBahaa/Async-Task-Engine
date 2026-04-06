#pragma once
/// @file pipeline.hpp
/// @brief Composable async pipeline — stage1 | stage2 | stage3 | ...
///
/// ═══════════════════════════════════════════════════════════════════
///  WHAT IS A PIPELINE?
/// ═══════════════════════════════════════════════════════════════════
///
/// A pipeline chains stages where each stage transforms its input
/// and passes the result to the next stage.  Each stage runs as
/// a task on the ThreadPool — stages execute concurrently.
///
/// Usage:
///   auto result = Pipeline<int>::create(pool, 42)
///       .then([](int x) { return x * 2; })       // stage 1
///       .then([](int x) { return x + 10; })      // stage 2
///       .then([](int x) { return std::to_string(x); })  // stage 3
///       .run();   // returns std::future<std::string>
///
///   std::string s = result.get();  // "94"
///
/// Each .then() adds a stage.  .run() submits the whole chain to
/// the pool and returns a future for the final result.

#include <ate/thread_pool.hpp>

#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace ate {

// ═══════════════════════════════════════════════════════════════════
//  Pipeline<T> — type-safe async pipeline builder
// ═══════════════════════════════════════════════════════════════════

template <typename T>
class Pipeline {
public:
    /// Create the first stage of a pipeline with an initial value.
    static Pipeline create(ThreadPool& pool, T initial) {
        return Pipeline(pool, std::move(initial));
    }

    /// Add a transformation stage.
    /// F: T → U  (can change the type)
    template <typename F>
    auto then(F&& func) -> Pipeline<std::invoke_result_t<F, T>> {
        using U = std::invoke_result_t<F, T>;

        // Capture the current chain and append a new stage
        auto prev_chain = std::move(chain_);
        auto prev_initial = std::move(initial_);

        // Build a new chain that runs the old chain, then applies func
        auto new_chain = [prev_chain = std::move(prev_chain),
                          prev_initial = std::move(prev_initial),
                          f = std::forward<F>(func)]() mutable -> U {
            T intermediate;
            if (prev_chain) {
                intermediate = prev_chain();
            } else {
                intermediate = std::move(prev_initial);
            }
            return f(std::move(intermediate));
        };

        Pipeline<U> next(pool_);
        next.set_chain(std::move(new_chain));
        return next;
    }

    /// Execute the pipeline on the pool, returning a future.
    std::future<T> run() {
        if (chain_) {
            auto chain = std::move(chain_);
            return pool_.submit(std::move(chain));
        }
        // No stages — just return the initial value
        auto val = std::move(initial_);
        return pool_.submit([v = std::move(val)]() mutable { return v; });
    }

private:
    explicit Pipeline(ThreadPool& pool)
        : pool_(pool), initial_{}, chain_(nullptr) {}

    Pipeline(ThreadPool& pool, T initial)
        : pool_(pool), initial_(std::move(initial)), chain_(nullptr) {}

    // Allow Pipeline<U> to access Pipeline<T> internals
    template <typename U>
    friend class Pipeline;

    void set_chain(std::move_only_function<T()> chain) {
        chain_ = std::move(chain);
    }

    ThreadPool& pool_;
    T initial_;
    std::move_only_function<T()> chain_;
};

} // namespace ate
