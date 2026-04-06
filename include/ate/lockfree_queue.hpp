#pragma once
/// @file lockfree_queue.hpp
/// @brief Bounded MPMC lock-free queue (Vyukov design).
///
/// ═══════════════════════════════════════════════════════════════════
///  HOW IT WORKS — the Vyukov bounded MPMC queue
/// ═══════════════════════════════════════════════════════════════════
///
/// This is a classic lock-free bounded queue by Dmitry Vyukov.
/// It uses a fixed-size ring buffer where each slot has its own
/// sequence counter (atomic).  The key insight:
///
///   • Producers and consumers never contend on the SAME atomic.
///   • Each slot's sequence tells you whether that slot is available
///     for writing (sequence == pos) or reading (sequence == pos + 1).
///
/// ── Ring Buffer Layout ──
///
///   Slot:  [  0  ][  1  ][  2  ][  3  ][  4  ][  5  ][  6  ][  7  ]
///   Seq:     0      1      2      3      4      5      6      7
///           ↑ enqueue_pos=0                     ↑ dequeue_pos=0
///
/// After enqueue(A):
///   Slot:  [ A  ][  1  ][  2  ][  3  ]...
///   Seq:     1      1      2      3  ...
///           ↑ seq bumped to 1         enqueue_pos → 1
///
/// ── Push Algorithm ──
///   1. Load enqueue_pos (relaxed)
///   2. Compute slot = buf[pos % capacity]
///   3. Load slot.sequence (acquire)
///   4. If seq == pos → slot is free; CAS enqueue_pos to pos+1
///   5. Construct the element in the slot
///   6. Store slot.sequence = pos + 1 (release)
///   ** If seq < pos → queue is FULL (return false)
///   ** If seq != pos → CAS failed, retry from step 1
///
/// ── Pop Algorithm ──
///   1. Load dequeue_pos (relaxed)
///   2. Compute slot = buf[pos % capacity]
///   3. Load slot.sequence (acquire)
///   4. If seq == pos + 1 → data is ready; CAS dequeue_pos to pos+1
///   5. Move element out of the slot
///   6. Store slot.sequence = pos + capacity (release) — marks slot as writable
///   ** If seq < pos + 1 → queue is EMPTY (return nullopt)
///   ** If seq != pos + 1 → CAS failed, retry from step 1
///
/// ── Memory Ordering ──
///   • enqueue_pos / dequeue_pos: relaxed load, acq_rel CAS
///     (only used to "claim" a slot; actual data visibility comes
///     from the slot's sequence counter)
///   • slot.sequence: acquire on load, release on store
///     This creates a happens-before from producer's store to
///     consumer's load — guaranteeing the data is visible.
///
/// ── False Sharing ──
///   enqueue_pos and dequeue_pos are on separate cache lines
///   (alignas(hardware_destructive_interference_size)).
///   Without this, producer and consumer threads would bounce
///   the same cache line back and forth — catastrophic for perf.
///
/// ── Capacity ──
///   Must be a power of 2.  We use bitwise AND instead of modulo:
///     pos & (capacity - 1)   instead of   pos % capacity
///   This is a single-cycle operation vs expensive integer division.
///
/// ── Trade-offs vs Mutex Queue ──
///   ✅ No syscalls (no futex/mutex), pure userspace spinning
///   ✅ No priority inversion
///   ✅ Better throughput under high contention
///   ❌ Bounded — must choose capacity up front
///   ❌ Spinning wastes CPU if queue is frequently empty/full
///   ❌ Harder to reason about correctness (no RAII lock)

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>     // hardware_destructive_interference_size
#include <optional>
#include <type_traits>
#include <vector>

namespace ate {

/// Bounded lock-free MPMC queue.
/// @tparam T  Element type (must be move-constructible).
///
/// Capacity must be a power of 2 and is fixed at construction.
template <typename T>
class LockFreeQueue {
    static_assert(std::is_move_constructible_v<T>,
                  "LockFreeQueue<T> requires T to be move-constructible");

public:
    /// Construct with the given capacity (rounded UP to next power of 2).
    explicit LockFreeQueue(std::size_t min_capacity)
        : capacity_(round_up_pow2(min_capacity))
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
    {
        assert(capacity_ >= 2 && "Capacity must be >= 2");

        // Initialise each slot's sequence to its index.
        // This marks every slot as "available for writing" at the
        // correct position.
        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }

        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    ~LockFreeQueue() = default;

    // Non-copyable, non-movable
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

    /// Try to enqueue an element.  Returns false if the queue is full.
    /// Lock-free (wait-free for uncontended case).
    bool try_push(T&& value) {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = buffer_[pos & mask_];
            std::size_t seq = slot.sequence.load(std::memory_order_acquire);

            auto diff = static_cast<std::ptrdiff_t>(seq) -
                        static_cast<std::ptrdiff_t>(pos);

            if (diff == 0) {
                // Slot is available for writing at this position.
                // Try to claim it by advancing enqueue_pos_.
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                {
                    // Won the CAS — we own this slot.
                    slot.data = std::move(value);
                    // Release: make the data visible to consumers.
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed — another producer took this slot.
                // pos was updated by CAS; retry.
            }
            else if (diff < 0) {
                // seq < pos → the slot hasn't been recycled yet.
                // This means the queue is FULL.
                return false;
            }
            else {
                // diff > 0 → another producer already claimed pos;
                // reload and try the next position.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Convenience overload for lvalues.
    bool try_push(const T& value) {
        T copy = value;
        return try_push(std::move(copy));
    }

    /// Try to dequeue an element.  Returns std::nullopt if empty.
    /// Lock-free (wait-free for uncontended case).
    std::optional<T> try_pop() {
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = buffer_[pos & mask_];
            std::size_t seq = slot.sequence.load(std::memory_order_acquire);

            auto diff = static_cast<std::ptrdiff_t>(seq) -
                        static_cast<std::ptrdiff_t>(pos + 1);

            if (diff == 0) {
                // Data is ready at this slot.
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                {
                    // Won the CAS — we own this slot.
                    T result = std::move(slot.data);
                    // Release: mark slot as available for the next round.
                    // Next valid write pos for this slot = pos + capacity.
                    slot.sequence.store(pos + capacity_,
                                        std::memory_order_release);
                    return result;
                }
                // CAS failed — another consumer took it.
            }
            else if (diff < 0) {
                // seq < pos + 1 → no data available → queue is EMPTY.
                return std::nullopt;
            }
            else {
                // diff > 0 → pos was stale; reload.
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Returns the capacity of the ring buffer.
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Approximate size (racy — useful for diagnostics only).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        auto enq = enqueue_pos_.load(std::memory_order_relaxed);
        auto deq = dequeue_pos_.load(std::memory_order_relaxed);
        return (enq >= deq) ? (enq - deq) : 0;
    }

private:
    /// Round up to the next power of 2 (or return n if already power of 2).
    static std::size_t round_up_pow2(std::size_t n) {
        if (n < 2) return 2;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    struct Slot {
        std::atomic<std::size_t> sequence;
        T data{};
    };

    // ── Cache-line alignment ────────────────────────────────────
    // Prevent false sharing between enqueue_pos and dequeue_pos.
    // On x86-64, a cache line is 64 bytes.  We hardcode this
    // rather than using std::hardware_destructive_interference_size
    // because GCC warns that value may vary across builds (-Winterference-size).
    static constexpr std::size_t kCacheLine = 64;

    const std::size_t       capacity_;
    const std::size_t       mask_;
    std::vector<Slot>       buffer_;

    // Each on its own cache line to avoid false sharing.
    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_;
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_;
};

} // namespace ate
