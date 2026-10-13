#pragma once

#include <cstdint>
#include <vector>

#include "dds/common/epoch.h"
#include "dds/common/status.h"
#include "dds/gc/planner.h"

namespace dds {

struct RepoInternals;

struct CompactCounters {
  uint64_t containers_deleted = 0;
  uint64_t containers_compacted = 0;
  uint64_t blobs_removed = 0;  // dead entries that left the index
  uint64_t blobs_copied = 0;   // live entries copied forward
  uint64_t bytes_copied = 0;
  uint64_t duplicates_dropped = 0;  // footer entries the index did not point at
};

// GC steps 4-6 up to retirement (ARCHITECTURE.md 4.3) for every container the plan deletes or
// compacts: decide each footer entry under its shard lock (RemoveIfDead), copy the kept blobs into
// GC's own containers, seal them durably, relocate the moved entries, publish the new containers,
// and only then retire the old ones (D22: nothing is deleted before a durable copy of everything
// live exists). A container planned for deletion that yields a kept entry (a backup referenced it
// after the plan) is compacted instead. Unlinking is ReclaimRetired's job.
Status Compact(RepoInternals& repo, const std::vector<PlannedContainer>& plan, GcRun run,
               Epoch e_min, CompactCounters& counters);

}  // namespace dds
