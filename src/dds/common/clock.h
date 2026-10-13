#pragma once

#include <atomic>
#include <cstdint>

namespace dds {

// Time source. Injectable so tests control time (TESTING.md section 1).
// - WallNanos: nanoseconds since the Unix epoch, for timestamps stored in files.
// - MonotonicNanos: a clock that never goes backwards, for durations and timeouts.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual int64_t WallNanos() const = 0;
  virtual int64_t MonotonicNanos() const = 0;
};

// The operating system's clocks.
class SystemClock final : public Clock {
 public:
  static const SystemClock& Instance();
  int64_t WallNanos() const override;
  int64_t MonotonicNanos() const override;
};

// A clock that moves only when Advance() is called. Thread-safe.
class ManualClock final : public Clock {
 public:
  ManualClock(int64_t wall_start, int64_t monotonic_start)
      : wall_(wall_start), monotonic_(monotonic_start) {}
  int64_t WallNanos() const override { return wall_.load(); }
  int64_t MonotonicNanos() const override { return monotonic_.load(); }
  void Advance(int64_t nanos) {
    wall_.fetch_add(nanos);
    monotonic_.fetch_add(nanos);
  }

 private:
  std::atomic<int64_t> wall_;
  std::atomic<int64_t> monotonic_;
};

}  // namespace dds
