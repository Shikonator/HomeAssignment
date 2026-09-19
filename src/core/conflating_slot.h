#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace md {

// One-deep mailbox keeping only the newest value: the aggregator -> subscriber
// hand-off. A slow subscriber loses resolution and nothing else -- it never
// back-pressures the book writer or slows another subscriber. Sound because
// every published value is ABSOLUTE state, not a delta.
//
// The mutex holds only a pointer swap. std::atomic<shared_ptr> is not lock-free
// on libstdc++ either, so there is no lock-free win available at this rate.
template <typename T>
class ConflatingSlot {
 public:
  // Never blocks. Returns false if the slot is stopped.
  bool Publish(std::shared_ptr<const T> value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) return false;
      if (pending_ != nullptr) ++conflated_;
      pending_ = std::move(value);
    }
    // Notify outside the lock.
    ready_.notify_one();
    return true;
  }

  // Blocks until a value arrives or the slot stops. nullptr once drained.
  std::shared_ptr<const T> WaitNext() {
    std::unique_lock<std::mutex> lock(mutex_);
    // Predicate form: immune to spurious wakeups and lost notifies.
    ready_.wait(lock, [this] { return pending_ != nullptr || stopped_; });
    if (pending_ == nullptr) return nullptr;
    return std::exchange(pending_, nullptr);
  }

  // Bounded wait; nullptr on timeout or stop. Stop() is process-wide but RPC
  // cancellation is per subscriber, and the sync gRPC server cannot interrupt a
  // thread parked in user code -- so an unbounded wait leaks a handler thread
  // whenever a client disconnects while the book is quiet.
  std::shared_ptr<const T> WaitNextFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, timeout, [this] { return pending_ != nullptr || stopped_; });
    if (pending_ == nullptr) return nullptr;
    return std::exchange(pending_, nullptr);
  }

  // Wakes every waiter so stream handlers unwind on shutdown.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    ready_.notify_all();
  }

  bool stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }

  // Number of states this subscriber never saw because it was too slow.
  std::uint64_t conflated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return conflated_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::shared_ptr<const T> pending_;
  bool stopped_ = false;
  std::uint64_t conflated_ = 0;
};

}  // namespace md
