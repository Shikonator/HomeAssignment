#pragma once

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace md {

// Bounded single-producer / single-consumer queue.
//
// One venue thread produces, the aggregator thread consumes, so no CAS is
// required: each side owns one index and publishes it with a release store the
// other side acquires. The two indices sit on separate cache lines, because
// sharing one line would make every producer store invalidate the consumer's
// copy and vice versa -- the classic false-sharing stall that makes a "lock
// free" queue slower than a mutex.
//
// FULLNESS POLICY IS THE CALLER'S. This queue only reports failure. It must
// never silently drop, because what flows through it are book DELTAS: losing
// one corrupts the book permanently and undetectably. The venue runner's
// response to a failed Push is to discard the batch and force a full resync,
// which is always safe. This is the exact opposite of the aggregator -> client
// direction, where messages carry absolute state and dropping them is both safe
// and desirable.
template <typename T>
class SpscRing {
 public:
  explicit SpscRing(std::size_t capacity)
      : capacity_(RoundUpPow2(capacity)), mask_(capacity_ - 1), slots_(capacity_) {}

  // Producer side only.
  bool Push(T&& value) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= capacity_) return false;
    slots_[head & mask_] = std::move(value);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer side only.
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
