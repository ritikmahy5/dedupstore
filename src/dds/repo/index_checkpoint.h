#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dds/common/result.h"
#include "dds/fs/fs.h"
#include "dds/repo/repo_layout.h"
#include "dds/store/types.h"

namespace dds {

struct RepoInternals;

// The index checkpoint (D104): a copy of the index in `<repo>/index.ckpt`, so open can
// take most entries from one file instead of reading every container's footer. It is derived and
// never the source of truth: open checks it against the container list, reads the footers of the
// containers it does not cover, and removes it if any check fails.

// Writes the checkpoint of `in`'s index, covering every container that is published and not
// retired when the scan starts, and returns their ids (increasing). It holds the GC mutex for the
// whole write, so no entry pointing into a covered container moves or leaves the index during the
// scan; sessions only add entries, and every entry of a covered container was in the index before
// that container was published (D49). The file is written in tmp/, synced, and renamed into
// place, and the repository directory is synced (ARCHITECTURE.md 7.1).
Result<std::vector<uint32_t>> WriteIndexCheckpoint(RepoInternals& in);

struct LoadedCheckpoint {
  std::vector<uint32_t> used;      // covered containers still listed: their entries were loaded
  uint32_t next_container_id = 0;  // open never allocates a container id below this
};

// Loads `<repo>/index.ckpt` into `in`, whose index must still be empty: the entries of every
// covered container that is in `listed` (increasing ids), using up to `threads` threads (0 =
// hardware concurrency, at most 16), and marks those containers published. Entries of covered
// containers that are no longer listed (GC deleted them) are skipped. On any error the file is
// unusable and `in` may be partly loaded, so the caller discards `in` (Repository::Open).
Result<LoadedCheckpoint> LoadIndexCheckpoint(RepoInternals& in, const std::vector<uint32_t>& listed,
                                             size_t threads);

// The checkpoint's next container id, after the checks of its trailer and tables (open uses it
// when it does not load the entries).
Result<uint32_t> ReadCheckpointNextId(Fs& fs, const RepoLayout& layout, const RepoId& repo_id);

// Removes the checkpoint, if any, and syncs the repository directory, so a checkpoint open could
// not use is never read again, even after a crash.
Status RemoveIndexCheckpoint(Fs& fs, const RepoLayout& layout);

}  // namespace dds
