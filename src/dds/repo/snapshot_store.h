#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/result.h"
#include "dds/fs/fs.h"
#include "dds/repo/repo_layout.h"
#include "dds/snapshot/manifest.h"

namespace dds {

// "<32 hex>.snap"
std::string ManifestFileName(const SnapshotId& id);

// The snapshots/ directory: the commit records. Listing a directory lists the snapshots, committing
// one is a rename, and removing one is an unlink (D15). Thread-safe: commits, removals, and
// listings are serialized by snapshots_mutex (ARCHITECTURE.md 5.2), which is held across their
// I/O; no lock above it in the order is ever held then.
class SnapshotStore {
 public:
  SnapshotStore(Fs& fs, RepoLayout layout) : fs_(fs), layout_(std::move(layout)) {}

  struct Listing {
    std::vector<SnapshotId> ids;       // sorted
    std::vector<std::string> ignored;  // names that are not "<32 hex>.snap" (verify reports them)
  };
  Result<Listing> List() DDS_EXCLUDES(mu_);

  // NotFound if the snapshot does not exist.
  Result<std::vector<std::byte>> ReadBytes(const SnapshotId& id);
  // Decodes the manifest and checks that its content hashes to its file name (ON_DISK_FORMAT.md
  // section 8): Corruption otherwise.
  Result<Manifest> Read(const SnapshotId& id);
  Result<bool> Exists(const SnapshotId& id);

  // ARCHITECTURE.md 7.3 step 3: write tmp/<id>.snap.tmp, sync, rename to snapshots/<id>.snap, sync
  // snapshots/. The rename is the commit point (G2). Committing a snapshot that already exists
  // succeeds without writing: same bytes, same id (replication's idempotent COMMIT).
  Result<SnapshotId> Commit(std::span<const std::byte> manifest_bytes) DDS_EXCLUDES(mu_);

  // ARCHITECTURE.md 7.4: unlink, then sync snapshots/. Its blobs become garbage for the next GC.
  Status Remove(const SnapshotId& id) DDS_EXCLUDES(mu_);

  // A full id, or a unique prefix of at least 6 lowercase hex digits (SPEC.md section 3).
  // InvalidArgument for anything shorter or not hex, or a prefix matching several snapshots;
  // NotFound if nothing matches.
  Result<SnapshotId> Resolve(std::string_view id_or_prefix);

 private:
  std::string PathOf(const SnapshotId& id) const;

  Fs& fs_;
  const RepoLayout layout_;
  OrderedMutex mu_{lock_level::kSnapshots, "snapshots"};
};

}  // namespace dds
