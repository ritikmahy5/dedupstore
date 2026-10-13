#pragma once

#include <cstdint>

#include "dds/common/result.h"
#include "dds/snapshot/manifest.h"
#include "dds/snapshot/restore_sink.h"

namespace dds {

struct RepoInternals;

struct RestoreOptions {
  bool verify = true;  // check BLAKE3 of every blob (D33); `--no-verify` turns it off
};

struct RestoreResult {
  uint64_t files = 0;
  uint64_t directories = 0;
  uint64_t symlinks = 0;
  uint64_t bytes = 0;  // file content written
};

// Restores snapshot `id` into `sink` (ARCHITECTURE.md 4.2): trees depth-first, files from their
// recipe pieces, every blob CRC-checked and (by default) BLAKE3-verified, each file's chunk lengths
// summed against its recorded size. A missing blob is Aborted if the snapshot was removed while
// restoring, otherwise Corruption naming the path.
Result<RestoreResult> RunRestore(RepoInternals& repo, const SnapshotId& id, RestoreSink& sink,
                                 const RestoreOptions& options);

}  // namespace dds
