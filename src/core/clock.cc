#include "src/core/clock.h"

#include <cstdio>
#include <ctime>

namespace md {

std::string FormatWallNs(std::int64_t wall_ns) {
  const std::time_t seconds = static_cast<std::time_t>(wall_ns / 1'000'000'000);
  const long nanos = static_cast<long>(wall_ns % 1'000'000'000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[48];
  const std::size_t date_len = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
  std::snprintf(buffer + date_len, sizeof(buffer) - date_len, ".%09ldZ", nanos);
  return std::string(buffer);
}

}  // namespace md
