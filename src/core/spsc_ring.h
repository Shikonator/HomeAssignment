#pragma once

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace md {

// Bounded single-producer / single-consumer queue. One venue thread produces,
// the aggregator consumes, so each side owns one index and no CAS is needed.
//
// NEVER MAKE THIS DROP ON OVERFLOW. It carries book DELTAS, and losing one
// corrupts the book permanently and undetectably. Push reports failure and the
// caller resyncs. (The aggregator -> subscriber direction is the opposite: that
// carries absolute state, so dropping is both safe and desirable.)
template <typename T>
class SpscRing {
 public:
  explicit SpscRing(std::size_t capacity)
      : capacity_(RoundUpPow2(capacity)), mask_(capacity_ - 1), slots_(capacity_) {}

  bool Push(T&& value) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= capacity_) return false;
    slots_[head & mask_] = std::move(value);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool Pop(T* out) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    *out = std::move(slots_[tail & mask_]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const { return capacity_; }
  std::size_t size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  bool empty() const { return size() == 0; }

 private:
  static std::size_t RoundUpPow2(std::size_t v) {
    std::size_t p = 1;
    while (p < v) p <<= 1;
    return p;
  }

  const std::size_t capacity_;
  const std::size_t mask_;
  std::vector<T> slots_;

  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace md
