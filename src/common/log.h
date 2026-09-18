#pragma once

#include <cstdio>
#include <mutex>
#include <string>

#include "src/core/clock.h"

namespace md {

// Deliberately tiny. Services log operational events -- connects, resyncs,
// venue state changes -- to stderr, leaving stdout as the clean data channel
// the assignment asks the publishers to write to. One mutex so lines from
// different venue threads never interleave mid-line in a combined compose log.
inline void Log(const char* component, const std::string& message) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  std::fprintf(stderr, "%s [%s] %s\n", FormatWallNs(WallNowNs()).c_str(), component,
               message.c_str());
  std::fflush(stderr);
}

}  // namespace md
