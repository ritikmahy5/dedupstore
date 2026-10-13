#include "dds/common/clock.h"

#include <gtest/gtest.h>

namespace dds {
namespace {

TEST(ClockTest, MonotonicNeverGoesBackwards) {
  const Clock& clock = SystemClock::Instance();
  int64_t prev = clock.MonotonicNanos();
  for (int i = 0; i < 1000; ++i) {
    int64_t now = clock.MonotonicNanos();
    EXPECT_GE(now, prev);
    prev = now;
  }
}

TEST(ClockTest, WallClockIsAfter2025) {
  // 2025-01-01T00:00:00Z in nanoseconds since the Unix epoch.
  constexpr int64_t k2025 = 1735689600LL * 1000000000LL;
  EXPECT_GT(SystemClock::Instance().WallNanos(), k2025);
}

TEST(ClockTest, ManualClockAdvancesOnlyWhenTold) {
  ManualClock clock(/*wall_start=*/100, /*monotonic_start=*/5);
  EXPECT_EQ(clock.WallNanos(), 100);
  EXPECT_EQ(clock.MonotonicNanos(), 5);
  clock.Advance(10);
  EXPECT_EQ(clock.WallNanos(), 110);
  EXPECT_EQ(clock.MonotonicNanos(), 15);
}

}  // namespace
}  // namespace dds
