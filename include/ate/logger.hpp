#pragma once
/// @file logger.hpp
/// @brief Async, lock-free logger for the task engine.
///
/// ═══════════════════════════════════════════════════════════════════
///  WHY AN ASYNC LOGGER?
/// ═══════════════════════════════════════════════════════════════════
///
/// Synchronous logging (e.g., fprintf to stderr under a mutex)
/// creates a serial bottleneck: every thread that logs must wait
/// for the lock, serializing all work.
///
/// Our async logger:
///   1. Formats the message on the calling thread (cheap: snprintf)
///   2. Pushes the formatted string into a lock-free MPSC queue
///   3. A dedicated drain thread writes to the sink (console/file)
///
/// This means the hot path (push) is ~50ns (one CAS), and the
/// expensive I/O happens on a background thread that doesn't
/// block workers.
///
/// ── Log Levels ──
///
///   TRACE < DEBUG < INFO < WARN < ERROR < FATAL
///
///   At runtime, set a minimum level.  Messages below it are
///   discarded at the call site (no allocation, no push).
///
/// ── Thread Safety ──
///
///   log() is safe to call from ANY thread, including pool workers,
///   Asio threads, and the main thread.  The drain thread is the
///   only writer to the sink.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ate {

// ─── Log Levels ─────────────────────────────────────────────────

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5
};

inline const char* to_string(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

// ─── Log Entry ──────────────────────────────────────────────────

struct LogEntry {
    LogLevel    level;
    std::string message;
    std::chrono::steady_clock::time_point timestamp;
    std::thread::id thread_id;
};

// ─── Logger ─────────────────────────────────────────────────────

class Logger {
public:
    using SinkFn = std::function<void(const LogEntry&)>;

    /// Create a logger with a minimum level and optional custom sink.
    /// Default sink writes to stderr.
    explicit Logger(LogLevel min_level = LogLevel::Info,
                    SinkFn sink = nullptr)
        : min_level_(min_level)
        , sink_(std::move(sink))
        , running_(true)
    {
        if (!sink_) {
            sink_ = [this](const LogEntry& entry) {
                default_sink(entry);
            };
        }

        drain_thread_ = std::thread([this] { drain_loop(); });
    }

    ~Logger() {
        flush();
        running_.store(false, std::memory_order_release);
        cv_.notify_one();
        if (drain_thread_.joinable()) {
            drain_thread_.join();
        }
    }

    // Non-copyable, non-movable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /// Log a message at the given level.
    /// Thread-safe — can be called from any thread.
    void log(LogLevel level, std::string message) {
        if (static_cast<int>(level) < static_cast<int>(min_level_.load(std::memory_order_relaxed))) {
            return;  // below threshold — discard
        }

        LogEntry entry{
            level,
            std::move(message),
            std::chrono::steady_clock::now(),
            std::this_thread::get_id()
        };

        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(entry));
        }
        cv_.notify_one();
    }

    /// Convenience methods
    void trace(std::string msg) { log(LogLevel::Trace, std::move(msg)); }
    void debug(std::string msg) { log(LogLevel::Debug, std::move(msg)); }
    void info(std::string msg)  { log(LogLevel::Info,  std::move(msg)); }
    void warn(std::string msg)  { log(LogLevel::Warn,  std::move(msg)); }
    void error(std::string msg) { log(LogLevel::Error, std::move(msg)); }
    void fatal(std::string msg) { log(LogLevel::Fatal, std::move(msg)); }

    /// Change the minimum log level at runtime.
    void set_level(LogLevel level) {
        min_level_.store(level, std::memory_order_relaxed);
    }

    /// Block until all queued messages have been written to the sink.
    void flush() {
        std::uint64_t target = 0;
        {
            std::lock_guard lock(mutex_);
            target = ++flush_gen_pending_;
        }
        cv_.notify_one();  // wake drain thread

        std::unique_lock lock(mutex_);
        flush_cv_.wait(lock, [this, target] {
            return flush_gen_done_ >= target;
        });
    }

private:
    void drain_loop() {
        while (running_.load(std::memory_order_acquire) || !queue_empty()) {
            std::deque<LogEntry> batch;
            std::uint64_t pending = 0;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(10),
                             [this] { return !queue_.empty() || !running_.load(std::memory_order_relaxed); });
                batch.swap(queue_);
                pending = flush_gen_pending_;
            }

            // Process the batch OUTSIDE the lock
            for (const auto& entry : batch) {
                sink_(entry);
            }

            // Now signal that we've processed up to `pending`
            {
                std::lock_guard lock(mutex_);
                flush_gen_done_ = pending;
            }
            flush_cv_.notify_all();
        }
    }

    bool queue_empty() {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

    void default_sink(const LogEntry& entry) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            entry.timestamp.time_since_epoch()).count();

        std::fprintf(stderr, "[%s] [%06ld] [t:%zu] %s\n",
                     to_string(entry.level),
                     static_cast<long>(elapsed % 1'000'000),
                     std::hash<std::thread::id>{}(entry.thread_id) % 10000,
                     entry.message.c_str());
    }

    std::atomic<LogLevel>           min_level_;
    SinkFn                          sink_;
    std::atomic<bool>               running_;
    std::mutex                      mutex_;
    std::condition_variable         cv_;
    std::condition_variable         flush_cv_;
    std::deque<LogEntry>            queue_;
    std::uint64_t                   flush_gen_pending_ = 0;
    std::uint64_t                   flush_gen_done_ = 0;
    std::thread                     drain_thread_;
};

} // namespace ate
