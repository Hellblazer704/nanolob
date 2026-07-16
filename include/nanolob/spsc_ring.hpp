#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

#include "nanolob/types.hpp"

namespace nanolob {

// Lock-free single-producer / single-consumer ring buffer.
//
// Correctness: the producer only writes tail_ and reads head_; the consumer
// only writes head_ and reads tail_. tail_ is published with release order
// after the slot write, and read with acquire order by the consumer, which
// makes the slot contents visible (and symmetrically for head_ / slot reuse).
//
// Performance:
//  - Capacity is a compile-time power of two: index masking folds to an AND.
//  - head_ and tail_ live on separate cache lines so the producer and
//    consumer cores never false-share their hot counters.
//  - Each side keeps a *cached* copy of the other side's counter next to its
//    own. The producer only re-reads head_ (a cross-core cache miss) when the
//    ring looks full against the cached value; in steady state push/pop touch
//    no shared line except the one being published. This is the classic
//    Rigtorp/LMAX optimization and is worth ~3x on throughput.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "slots are copied raw");

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer side. Returns false if the ring is full.
  bool try_push(const T& value) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ >= Capacity) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ >= Capacity) return false;
    }
    slots_[tail & kMask] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false if the ring is empty.
  bool try_pop(T& out) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) return false;
    }
    out = slots_[head & kMask];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer-side snapshot; exact for the consumer, a lower bound elsewhere.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) -
                                    head_.load(std::memory_order_acquire));
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  static constexpr std::uint64_t kMask = Capacity - 1;

  // Consumer line: head_ (written by consumer) + consumer's cache of tail_.
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_ = 0;

  // Producer line: tail_ (written by producer) + producer's cache of head_.
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_ = 0;

  alignas(kCacheLine) T slots_[Capacity];
};

}  // namespace nanolob
