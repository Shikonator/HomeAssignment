#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace md {

// Wall-clock nanoseconds. Used for timestamps that leave the process.
inline std::int64_t WallNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Monotonic nanoseconds. Used for every duration and every staleness decision,
// because the wall clock can step and an exchange's clock is not ours to trust.
inline std::int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// RFC3339 with nanosecond precision, e.g. 2026-09-17T22:41:03.123456789Z.
std::string FormatWallNs(std::int64_t wall_ns);

}  // namespace md
