#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "dds/common/clock.h"
#include "dds/common/result.h"
#include "dds/repo/ingest_session.h"
#include "dds/snapshot/manifest.h"
#include "dds/snapshot/source_tree.h"

namespace dds {

struct RepoInternals;

// The walker hands files to the workers in batches (D102): one queue operation moves up to
// kFileBatchFiles files, so the walker and the workers take the queue's lock once per batch
// instead of once per file. A batch also closes once its files' listed sizes reach
// kFileBatchBytes, so a run of large files still spreads across workers.
inline constexpr size_t kFileBatchFiles = 64;
inline constexpr uint64_t kFileBatchBytes = uint64_t{16} << 20;
// Files at least this large are split across workers by default (D109): about a tenth of a
// second of one worker's time, long enough that sharing the file pays for queueing its segments.
inline constexpr uint64_t kDefaultSplitFileBytes = uint64_t{64} << 20;

struct BackupOptions {
  std::string source_path;  // recorded in the manifest
  std::string tag;
  std::string host;
  size_t threads = 1;  // file workers, each with its own container writer (D31); at least 1
  // Files unchanged since this snapshot are not read: their pieces are reused (D106).
  std::optional<SnapshotId> parent;
  // A file at least this large is chunked by one worker and stored by all of them, in segments
  // (D109); UINT64_MAX never splits. Splitting does not change what is stored.
  uint64_t split_file_bytes = kDefaultSplitFileBytes;
};

// Where a backup's time went (BENCHMARKS.md E3). The per-file steps are summed over the workers, so
// with N workers they can add up to N times the wall time; the others are the calling thread's.
struct BackupTimings {
  uint64_t walk_ns = 0;        // listing the tree (overlaps the workers)
  uint64_t read_chunk_ns = 0;  // reading files and finding chunk boundaries
  uint64_t hash_ns = 0;        // BLAKE3 of each chunk
  uint64_t store_ns = 0;       // lookups, buffering, sealing full containers, recipe pieces
  uint64_t reuse_ns = 0;       // looking up and stamping the blobs of files reused from the parent
  uint64_t trees_ns = 0;       // building tree blobs after the workers are joined
  uint64_t commit_ns = 0;      // sealing what is left and writing the manifest
};

struct BackupResult {
  SnapshotId id{};
  Manifest manifest;
  IngestCounters ingest;
  BackupTimings timings;
  uint64_t skipped_special = 0;        // devices, FIFOs, sockets (SPEC.md section 4)
  uint64_t changed_while_reading = 0;  // files whose size changed between listing and reading
  std::optional<SnapshotId> parent;    // as given in BackupOptions
  uint64_t files_reused = 0;           // files not read: unchanged since the parent (D106)
  uint64_t bytes_reused = 0;           // their logical bytes
  uint64_t files_split = 0;            // files stored in segments by several workers (D109)
  // Their runs queued for another worker; the splitter may still store a few itself, when the
  // idle thread found other work meanwhile.
  uint64_t segments_queued = 0;
};

// Backs up `source` as one snapshot (ARCHITECTURE.md 4.1): the calling thread lists every
// directory, recording its entries and queueing each regular file; `options.threads` workers store
// the files (chunk, fingerprint, deduplicate, recipe pieces); then the calling thread builds tree
// blobs bottom-up and commits the manifest. The root fingerprint does not depend on the thread
// count or on splitting (G11, D109). Any error fails the backup and commits nothing. `source` must
// be safe to read from several threads.
Result<BackupResult> RunBackup(RepoInternals& repo, SourceTree& source,
                               const BackupOptions& options, const Clock& clock);

}  // namespace dds
