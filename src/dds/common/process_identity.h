#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dds {

// Who a process is, for the repository lock (D16): host name, pid, and the process's start time,
// which tells a live holder apart from a later process that reused its pid. Injectable, so the
// crash harness can declare a crashed holder dead (TESTING.md 3.6).
class ProcessIdentity {
 public:
  virtual ~ProcessIdentity() = default;
  virtual std::string Host() const = 0;
  virtual uint64_t Pid() const = 0;
  virtual uint64_t PidStart() const = 0;
  // True if a process with this pid is running on this host and started at `pid_start`.
  virtual bool IsAlive(uint64_t pid, uint64_t pid_start) const = 0;
};

// The real identity of the calling process.
class PosixProcessIdentity final : public ProcessIdentity {
 public:
  PosixProcessIdentity();
  std::string Host() const override { return host_; }
  uint64_t Pid() const override { return pid_; }
  uint64_t PidStart() const override { return pid_start_; }
  bool IsAlive(uint64_t pid, uint64_t pid_start) const override;

 private:
  std::string host_;
  uint64_t pid_ = 0;
  uint64_t pid_start_ = 0;
};

// The start time of process `pid`, or nullopt if no such process exists (ON_DISK_FORMAT.md
// section 4). Linux: field 22 (`starttime`, clock ticks since boot) of /proc/<pid>/stat. macOS:
// kp_proc.p_starttime from sysctl(KERN_PROC_PID), in microseconds since the Unix epoch. Values are
// only compared on the machine that produced them.
std::optional<uint64_t> ProcessStartTime(uint64_t pid);

}  // namespace dds
