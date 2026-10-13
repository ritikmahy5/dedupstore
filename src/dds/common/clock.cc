#include "dds/common/clock.h"

#include <chrono>

namespace dds {

const SystemClock& SystemClock::Instance() {
  static const SystemClock clock;
  return clock;
}

int64_t SystemClock::WallNanos() const {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t SystemClock::MonotonicNanos() const {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace dds
