#pragma once

#include <cstdint>

#include "dds/common/epoch.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/thread_annotations.h"

namespace dds {

// What GC runs did, summed over the runs in this process (`stats` reports it, D81).
struct GcTotals {
  uint64_t runs = 0;
  uint64_t containers_deleted = 0;
  uint64_t containers_compacted = 0;
  uint64_t blobs_removed = 0;
  uint64_t blobs_copied = 0;
  uint64_t bytes_copied = 0;
  uint64_t containers_reclaimed = 0;
  uint64_t bytes_reclaimed = 0;
};

// GC's state that outlives one run, kept with the repository so every GcRunner on it shares it.
// gc_mutex (the lowest lock level, 10) is held for a whole run: one GC at a time (4.3 step 1).
struct GcState {
  OrderedMutex mu{lock_level::kGc, "gc"};
  GcRun last_run DDS_GUARDED_BY(mu) = 0;  // runs number from 1 (6.1)
  GcTotals totals DDS_GUARDED_BY(mu);
};

}  // namespace dds
