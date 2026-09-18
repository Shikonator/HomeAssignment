#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <random>

namespace md {

// Exponential backoff with full jitter.
//
// Extracted as its own type purely so it can be tested: it is arithmetic over
// one member, and when it lived inline in the reconnect path a reset in the
// wrong place silently pinned every retry at the initial delay.
class ExponentialBackoff {
 public:
  ExponentialBackoff(std::chrono::milliseconds initial, std::chrono::milliseconds max)
      : initial_(initial), max_(max), current_(initial) {}

  // Returns the delay to wait, then doubles for next time.
  //
  // Full jitter rather than a fixed delay: without it, every venue that dropped
  // on the same network blip retries in lockstep forever.
  std::chrono::milliseconds Next() {
    const std::chrono::milliseconds window = current_;
    current_ = std::min(max_, current_ * 2);
    std::uniform_int_distribution<long long> spread(window.count() / 2, window.count());
    return std::chrono::milliseconds(spread(rng_));
  }

  // The undithered delay Next() would draw from. Exposed for tests and logs.
  std::chrono::milliseconds peek() const { return current_; }

  // Call ONLY once the connection is genuinely healthy, never on the attempt.
  // An endpoint that accepts TCP and drops you a second later -- which is what
  // a rate-limited venue does -- would otherwise reset the backoff on every
  // cycle and never actually back off.
  void Reset() { current_ = initial_; }

 private:
  std::chrono::milliseconds initial_;
  std::chrono::milliseconds max_;
  std::chrono::milliseconds current_;
  std::mt19937 rng_{std::random_device{}()};
};

// A sliding-window rate budget: at most N events per window.
//
// Guards the Binance REST snapshot, whose depth-5000 response carries heavy
// request weight. Without it a resync loop earns an IP ban, which then presents
// as a network outage.
class RateBudget {
 public:
  RateBudget(int max_events, std::chrono::steady_clock::duration window)
      : max_events_(max_events), window_(window) {}

  bool TryConsume(std::chrono::steady_clock::time_point now) {
    while (!events_.empty() && now - events_.front() > window_) events_.pop_front();
    if (static_cast<int>(events_.size()) >= max_events_) return false;
    events_.push_back(now);
    return true;
  }

  int used() const { return static_cast<int>(events_.size()); }

 private:
  int max_events_;
  std::chrono::steady_clock::duration window_;
  std::deque<std::chrono::steady_clock::time_point> events_;
};

}  // namespace md
