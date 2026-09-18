#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace md {

// A one-deep mailbox that keeps only the newest value.
//
// This is the aggregator -> subscriber hand-off. Each subscriber owns one slot.
// Publishing overwrites whatever the subscriber has not collected yet, so a
// subscriber that cannot keep up loses resolution and nothing else: it never
// applies back-pressure to the book writer, never grows an unbounded queue, and
// never slows another subscriber down. Conflation is sound here precisely
// because every published value is ABSOLUTE state rather than a delta.
//
// On the mutex: the critical section is a pointer swap. At the ~30-60 Hz these
// feeds actually produce, an uncontended mutex costs tens of nanoseconds, and
// std::atomic<std::shared_ptr<T>> is not lock-free on libstdc++ either -- it
// uses a spinlock pool, i.e. a lock with worse semantics. There is no
// lock-free win available at this rate; what matters is that the writer is
// never blocked, and it is not.
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
    // Notify outside the lock so the woken consumer does not immediately block
    // on a mutex we still hold.
    ready_.notify_one();
    return true;
  }

  // Blocks until a value is available or the slot is stopped.
  // Returns nullptr once stopped and drained.
  std::shared_ptr<const T> WaitNext() {
    std::unique_lock<std::mutex> lock(mutex_);
    // Predicate form: immune to spurious wakeups and to a notify that lands
    // between the check and the wait.
    ready_.wait(lock, [this] { return pending_ != nullptr || stopped_; });
    if (pending_ == nullptr) return nullptr;
    return std::exchange(pending_, nullptr);
  }

  // Bounded wait. Returns nullptr on timeout as well as on stop.
  //
  // Needed because Stop() is process-wide but RPC cancellation is per
  // subscriber. The synchronous gRPC server cannot interrupt a thread parked in
  // user code: a cancelled handler only notices on its next Write(). With an
  // unbounded wait, a subscriber that disconnects while the book is quiet --
  // every venue stale, or a long min_interval_micros -- parks a handler thread
  // forever, and repeated connect/disconnect leaks the bounded sync pool until
  // new RPCs stop being served. Polling at a fraction of a second costs nothing
  // at these rates and closes that leak.
  std::shared_ptr<const T> WaitNextFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock, timeout, [this] { return pending_ != nullptr || stopped_; });
    if (pending_ == nullptr) return nullptr;
    return std::exchange(pending_, nullptr);
  }

  // Wakes every waiter so stream handlers can unwind. Without this, gRPC
  // handler threads park forever, the process ignores SIGTERM, and
  // `docker compose down` degenerates into a SIGKILL timeout per service.
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
