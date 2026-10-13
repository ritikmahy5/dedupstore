#pragma once

#include <cstdint>
#include <vector>

#include "dds/common/epoch.h"
#include "dds/common/status.h"
#include "dds/snapshot/manifest.h"

namespace dds {

struct RepoInternals;

struct MarkCounters {
  uint64_t snapshots = 0;  // walked (a snapshot removed after the listing is skipped)
  uint64_t blobs = 0;      // distinct blobs marked
};

// GC's mark phase (ARCHITECTURE.md 4.3 step 2): marks with `run` every blob reachable from the
// listed snapshots. Fails closed: a reachable fingerprint missing from the index, a blob of the
// wrong type, or a tree or recipe that cannot be read or parsed is an error, and the caller must
// then delete nothing. A tree already marked in this run is not walked again, which also stops a
// tree that (through a damaged index) contains itself. GC reads need no ReadGuard: only GC unlinks
// containers, and it does not while marking.
Status MarkAll(RepoInternals& repo, GcRun run, const std::vector<SnapshotId>& snapshots,
               MarkCounters& counters);

}  // namespace dds
