#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

#include "quotemesh/types.hpp"

namespace qm {

// Bounded lock-free single-producer/single-consumer ring buffer.
//
// - Capacity is rounded up to a power of two so indices wrap with a mask.
// - Producer and consumer indices live on separate cache lines to avoid false sharing.
// - Each side keeps a cached copy of the other side's index and only reloads the shared
//   atomic when the cached value says the queue looks full/empty, which keeps cross-core
//   cache traffic to a minimum under load.
template <typename T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue holds trivially copyable types");

 public:
  explicit SpscQueue(std::size_t capacity) {
    if (capacity < 2) throw std::invalid_argument("SpscQueue capacity must be >= 2");
    std::size_t cap = 1;
    while (cap < capacity) cap <<= 1;
    mask_ = cap - 1;
    slots_ = std::make_unique<T[]>(cap);
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  std::size_t capacity() const noexcept { return mask_ + 1; }

  // Producer side.
  bool try_push(const T& v) noexcept {
    const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
    if (tail - prod_.head_cache > mask_) {
      prod_.head_cache = cons_.head.load(std::memory_order_acquire);
      if (tail - prod_.head_cache > mask_) return false;
    }
    slots_[tail & mask_] = v;
    prod_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer side.
  bool try_pop(T& out) noexcept {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    if (head == cons_.tail_cache) {
      cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);
      if (head == cons_.tail_cache) return false;
    }
    out = slots_[head & mask_];
    cons_.head.store(head + 1, std::memory_order_release);
    return true;
  }

  // Pop up to `max` items into `out`; returns count. Publishes the new head once.
  std::size_t pop_batch(T* out, std::size_t max) noexcept {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    std::size_t avail = cons_.tail_cache - head;
    if (avail == 0) {
      cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);
      avail = cons_.tail_cache - head;
      if (avail == 0) return 0;
    }
    const std::size_t n = avail < max ? avail : max;
    for (std::size_t i = 0; i < n; ++i) out[i] = slots_[(head + i) & mask_];
    cons_.head.store(head + n, std::memory_order_release);
    return n;
  }

  // Approximate size; safe to call from any thread for monitoring.
  std::size_t size_approx() const noexcept {
    return prod_.tail.load(std::memory_order_acquire) - cons_.head.load(std::memory_order_acquire);
  }

 private:
  struct alignas(kCacheLine) Producer {
    std::atomic<std::size_t> tail{0};
    std::size_t head_cache{0};
  };
  struct alignas(kCacheLine) Consumer {
    std::atomic<std::size_t> head{0};
    std::size_t tail_cache{0};
  };

  Producer prod_;
  Consumer cons_;
  std::size_t mask_{0};
  std::unique_ptr<T[]> slots_;
};

}  // namespace qm
