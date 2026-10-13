#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>

#include "dds/common/process_identity.h"

namespace dds::testing {

// A ProcessIdentity whose host, pid, start time, and set of live processes a test chooses. The
// crash harness uses it to declare a crashed holder dead, so its lock is taken over (TESTING.md
// 3.6). `on_is_alive` lets a test act at the moment the lock code checks liveness, for example to
// simulate another process winning a takeover race.
class FakeProcessIdentity final : public ProcessIdentity {
 public:
  FakeProcessIdentity(std::string host, uint64_t pid, uint64_t pid_start)
      : host_(std::move(host)), pid_(pid), pid_start_(pid_start) {
    alive_.insert({pid, pid_start});
  }

  std::string Host() const override { return host_; }
  uint64_t Pid() const override { return pid_; }
  uint64_t PidStart() const override { return pid_start_; }
  bool IsAlive(uint64_t pid, uint64_t pid_start) const override {
    if (on_is_alive) on_is_alive();
    return alive_.count({pid, pid_start}) > 0;
  }

  void SetAlive(uint64_t pid, uint64_t pid_start, bool alive) {
    if (alive) {
      alive_.insert({pid, pid_start});
    } else {
      alive_.erase({pid, pid_start});
    }
  }

  std::function<void()> on_is_alive;

 private:
  std::string host_;
  uint64_t pid_;
  uint64_t pid_start_;
  std::set<std::pair<uint64_t, uint64_t>> alive_;
};

}  // namespace dds::testing
