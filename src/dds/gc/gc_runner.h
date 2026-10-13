#pragma once

#include <cstdint>

#include "dds/common/epoch.h"
#include "dds/common/result.h"
#include "dds/gc/compactor.h"
#include "dds/gc/marker.h"

namespace dds {

class Repository;
struct RepoInternals;

struct GcOptions {
  double threshold = 0.5;  // in (0, 1]: compact containers whose live fraction is below it
  bool dry_run = false;    // plan and report; change nothing (no unlink, no copy, no index edit)
};

// What one run found and did.
struct GcReport {
  GcRun run = 0;
  bool dry_run = false;
  MarkCounters mark;
  uint64_t candidates = 0;
  uint64_t candidate_payload_bytes = 0;
  uint64_t live_bytes = 0;  // of the candidates, by the plan
  uint64_t planned_delete = 0;
  uint64_t planned_compact = 0;
  uint64_t planned_keep = 0;
  CompactCounters compact;  // zero in a dry run
  uint64_t containers_reclaimed = 0;
  uint64_t bytes_reclaimed = 0;  // container file bytes unlinked by this run
  double mark_seconds = 0;
  double plan_seconds = 0;
  double compact_seconds = 0;
};

// Runs garbage collection (ARCHITECTURE.md 4.3) on an open repository while backups, restores, and
// replication continue in the same process; section 6 holds the safety argument. One run at a
// time per repository (gc_mutex). Any marking error fails the run before anything is deleted.
// Depends on the repository's internals (D53): the index, the containers, the epochs, the snapshot
// directory.
class GcRunner {
 public:
  explicit GcRunner(Repository& repo);
  Result<GcReport> Run(const GcOptions& options);

 private:
  RepoInternals& repo_;
};

}  // namespace dds
