#pragma once

#include <array>
#include <cstdint>

#include "dds/common/result.h"
#include "dds/repo/gc_state.h"

namespace dds {

struct RepoInternals;

// What `dds stats` reports (SPEC.md sections 1 and 3).
struct RepoStats {
  uint64_t snapshots = 0;
  uint64_t logical_bytes = 0;  // sum over live snapshots of their file sizes
  // Distinct data chunks reachable from a snapshot: their content bytes, and those bytes as stored,
  // which compression (D107) makes smaller.
  uint64_t unique_data_bytes = 0;
  uint64_t stored_data_bytes = 0;
  uint64_t physical_bytes = 0;   // sum of published container file sizes
  double dedup_ratio = 0;        // logical / unique data bytes; 0 if there are none
  double compression_ratio = 0;  // unique / stored data bytes; 1 without compression
  uint64_t containers = 0;
  uint64_t blobs = 0;  // distinct fingerprints in the index
  uint64_t duplicate_copies = 0;
  uint64_t index_memory_bytes = 0;
  // Containers by live fraction (reachable canonical payload / payload): bucket i holds fractions
  // in [i/10, (i+1)/10), and bucket 9 also holds 1.0.
  std::array<uint64_t, 10> live_fraction_histogram{};
  // GC runs in this process (D81); a fresh `dds stats` process reports zeros.
  GcTotals gc;
};

// Walks every snapshot to find which blobs are live. A snapshot that cannot be walked fails with
// Corruption naming it: `verify` reports the details.
Result<RepoStats> ComputeStats(RepoInternals& repo);

}  // namespace dds
