#pragma once

#include <cstdint>
#include <vector>

#include "dds/common/epoch.h"

namespace dds {

struct RepoInternals;

// A container GC may act on, with the sizes the plan needs.
struct Candidate {
  uint32_t id = 0;
  uint64_t payload_bytes = 0;
  uint64_t file_bytes = 0;
};

enum class PlanAction { kKeep, kCompact, kDelete };

struct PlannedContainer {
  Candidate candidate;
  uint64_t live_bytes = 0;  // payload of entries marked in this run or protected by their stamp
  PlanAction action = PlanAction::kKeep;
};

// GC's plan (ARCHITECTURE.md 4.3 step 3): scans every index shard and sums, per candidate, the
// bytes of entries that are canonical there and marked in `run` or protected (`last_ref >= e_min`;
// a duplicate copy is never canonical, so it counts as dead). Live fraction 0 -> delete; below
// `threshold` -> compact; otherwise keep. Sorted by container id.
std::vector<PlannedContainer> MakePlan(RepoInternals& repo,
                                       const std::vector<Candidate>& candidates, GcRun run,
                                       Epoch e_min, double threshold);

}  // namespace dds
