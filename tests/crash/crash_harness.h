#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dds/fs/crash_sim_fs.h"
#include "dds/snapshot/manifest.h"
#include "dds/snapshot/mem_node.h"

namespace dds::testing {

// What a scenario finished before the crash point, recorded by the scenario as each step returns.
// It is the model the post-crash repository is checked against.
struct ScenarioLog {
  bool init_returned = false;
  std::map<SnapshotId, MemTree> committed;  // backups that returned OK and were not removed since
  std::set<SnapshotId> removed;             // rm returned OK
  std::optional<MemTree> backup_in_flight;  // a backup that started and has not returned OK
  std::optional<SnapshotId> rm_in_flight;   // an rm that started and has not returned OK
};

// A scenario runs repository operations on `fs` (repository path kRepoPath) until one fails, and
// keeps `log` up to date. It returns the first error, or OK when it ran to the end.
using Scenario = std::function<Status(CrashSimFs& fs, ScenarioLog& log)>;

inline constexpr const char* kRepoPath = "/r";

struct CrashReport {
  uint64_t crash_points = 0;  // K + 1: a crash after each of the K mutating calls, and before any
  uint64_t states = 0;        // crash_points x seeds post-crash disks checked
  uint64_t distinct_states = 0;  // of those, how many differed in content
  // How often each allowed outcome was seen ("in-flight backup present", "removal undone", ...),
  // so a test can check that both sides of each either-or were explored.
  std::map<std::string, uint64_t> outcomes;
  std::vector<std::string> violations;  // "crash point n, seed s: what was wrong"
};

// TESTING.md 3.6: runs `scenario` once to count its mutating calls K, then for each crash point
// n in 0..K reruns it crashing after call n, materializes `seeds` post-crash disks, and checks
// each: the repository opens (or, if init never returned, is absent and can be initialized), tmp/
// is empty, verify --deep finds nothing, every committed snapshot restores exactly, a snapshot in
// flight is absent or exact, and removed snapshots stay removed.
CrashReport ExploreCrashes(const Scenario& scenario, uint64_t seeds = 20);

}  // namespace dds::testing
