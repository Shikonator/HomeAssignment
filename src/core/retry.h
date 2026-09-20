#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <random>

namespace md {

// Exponential backoff with full jitter.
class ExponentialBackoff {
 public:
  ExponentialBackoff(std::chrono::milliseconds initial, std::chrono::milliseconds max)
      : initial_(initial), max_(max), current_(initial) {}

  // Jittered, so venues dropped by one network blip do not retry in lockstep.
  std::chrono::milliseconds Next() {
    const std::chrono::milliseconds window = current_;
    current_ = std::min(max_, current_ * 2);
    std::uniform_int_distribution<long long> spread(window.count() / 2, window.count());
    return std::chrono::milliseconds(spread(rng_));
  }

  // The undithered window Next() draws from.
  std::chrono::milliseconds peek() const { return current_; }

  // ONLY once the connection is genuinely healthy, never on the attempt. A
  // venue that accepts TCP then drops you would otherwise never back off.
  void Reset() { current_ = initial_; }

 private:
  std::chrono::milliseconds initial_;
  std::chrono::milliseconds max_;
  std::chrono::milliseconds current_;
  std::mt19937 rng_{std::random_device{}()};
};

// At most N events per window. Guards the Binance REST snapshot: an
// unthrottled resync loop earns an IP ban that looks like a network outage.
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
