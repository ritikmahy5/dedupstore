#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dds/chunk/chunker.h"
#include "dds/common/clock.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/process_identity.h"
#include "dds/common/result.h"
#include "dds/fs/fs.h"
#include "dds/index/chunk_index.h"
#include "dds/repo/backup.h"
#include "dds/repo/epoch_manager.h"
#include "dds/repo/gc_state.h"
#include "dds/repo/repo_config.h"
#include "dds/repo/repo_layout.h"
#include "dds/repo/repo_lock.h"
#include "dds/repo/restorer.h"
#include "dds/repo/snapshot_store.h"
#include "dds/repo/stats.h"
#include "dds/repo/verifier.h"
#include "dds/snapshot/tree.h"
#include "dds/store/container_set.h"
#include "dds/store/container_writer.h"

namespace dds {

// What `dds init` takes (SPEC.md section 3); the rest of RepoConfig is chosen at init.
struct RepoParams {
  ChunkerParams chunker;
  uint32_t container_target = kDefaultContainerTarget;
  CompressionParams compression;  // none by default (D107)
};

struct OpenOptions {
  std::string command = "dds";   // recorded in the lock file
  size_t open_threads = 0;       // footer-reading threads; 0 = hardware concurrency (at most 16)
  const Clock* clock = nullptr;  // nullptr = SystemClock
  // The index checkpoint (D104). Off, open reads every footer, as E9 measures it.
  bool use_index_checkpoint = true;
  // Close writes a new checkpoint when at least `checkpoint_stale_containers` containers are
  // published but not covered by the current one, or covered but deleted since.
  bool write_index_checkpoint = true;
  size_t checkpoint_stale_containers = 256;
};

// What open found (ARCHITECTURE.md 7.6). `verify` reports these; open itself only skips.
struct OpenReport {
  std::vector<std::string> bad_containers;  // "<path>: <error>" for each container skipped
  std::vector<std::string> ignored_files;   // names in containers/ that are not container files
  uint64_t containers = 0;                  // valid containers loaded
  uint64_t blobs = 0;                       // distinct fingerprints in the index
  uint64_t duplicate_copies = 0;            // footer entries beyond the first copy (D32)
  uint64_t checkpoint_containers = 0;       // of `containers`, those loaded from the checkpoint
  uint64_t footers_read = 0;                // containers whose footers were read
  std::string checkpoint_rejected;          // why a checkpoint present was removed, if it was
};

// The parts of an open repository. Repository owns them; `gc` and `repl` reach them through
// Repository::Internals() (D53).
struct RepoInternals {
  RepoInternals(Fs& fs, RepoLayout layout, RepoConfig config,
                const Clock& clock = SystemClock::Instance());

  // A writer for this repository's containers (one per thread, D31).
  ContainerWriter NewWriter();
  // Publish half of the seal protocol (ARCHITECTURE.md 4.1): index the entries, then mark the
  // container published. In that order a container becomes a GC candidate only after every entry
  // it holds is findable (D49).
  // `now` is the caller's epoch reading, taken after its session registered (6.1). Returns how many
  // entries another session had already published (duplicates, D32).
  size_t PublishSealed(const SealedContainer& sealed, Epoch now);

  Fs& fs;
  const Clock& clock;
  const RepoLayout layout;
  const RepoConfig config;
  ContainerSet containers;
  ChunkIndex index;
  EpochManager epochs;
  SnapshotStore snapshots;
  std::unique_ptr<Chunker> chunker;
  GcState gc;
};

// A snapshot as `dds snapshots` lists it.
struct SnapshotInfo {
  SnapshotId id{};
  Manifest manifest;
};

// A repository: created with Init, then opened by exactly one process at a time (D16).
class Repository {
 public:
  // ARCHITECTURE.md 7.7: `path` must not exist, or be empty, or hold only what a crashed init
  // leaves (empty tmp/, containers/, snapshots/ and possibly tmp/config.tmp); otherwise
  // AlreadyExists. The rename of `config` is the commit point (D52).
  // Returns the new repository's config.
  static Result<RepoConfig> Init(Fs& fs, const std::string& path, const RepoParams& params,
                                 const Clock& clock = SystemClock::Instance());

  // ARCHITECTURE.md 7.6: lock, parse config, empty tmp/, read every container's footer in
  // parallel, rebuild the index (highest container id wins, D12). NotFound without a config.
  static Result<std::unique_ptr<Repository>> Open(Fs& fs, const std::string& path,
                                                  const ProcessIdentity& self,
                                                  const OpenOptions& options = {});

  ~Repository();
  Repository(const Repository&) = delete;
  Repository& operator=(const Repository&) = delete;

  // Unlinks containers a GC retired (no reader can remain once the caller is done with the
  // repository), then releases the lock. The destructor calls it if needed.
  Status Close();

  // ARCHITECTURE.md 4.1. Several backups may run at once in one process (D32).
  Result<BackupResult> Backup(SourceTree& source, const BackupOptions& options);
  // ARCHITECTURE.md 4.2. NotFound if the snapshot does not exist.
  Result<RestoreResult> Restore(const SnapshotId& id, RestoreSink& sink,
                                const RestoreOptions& options);
  // Sorted by creation time, then id. A manifest that fails its checks fails the listing.
  Result<std::vector<SnapshotInfo>> ListSnapshots();
  // A full id or a unique prefix of at least 6 hex digits (SPEC.md section 3).
  Result<SnapshotId> ResolveSnapshot(std::string_view id_or_prefix);
  // The entries of the directory at `path` inside a snapshot ("" or "/" is the root). NotFound if
  // the path does not exist or is not a directory.
  Result<std::vector<TreeEntry>> ListPath(const SnapshotId& id, std::string_view path);
  // ARCHITECTURE.md 7.4. The space comes back at the next gc.
  Status RemoveSnapshot(const SnapshotId& id);
  Result<VerifyReport> Verify(const VerifyOptions& options);
  Result<RepoStats> Stats();
  // Writes the index checkpoint now (D104), whether or not it is stale. Safe while other
  // operations run: it holds the GC mutex for the write.
  Status WriteIndexCheckpoint();

  const RepoConfig& config() const { return internals_->config; }
  const OpenReport& open_report() const { return report_; }
  RepoInternals& Internals() { return *internals_; }

  // Only Open can make a Key, so only Open can construct a Repository (the constructor is public
  // so std::make_unique can call it).
  class Key {
    friend class Repository;
    Key() = default;
  };
  Repository(Key /*key*/, std::unique_ptr<RepoLock> lock, std::unique_ptr<RepoInternals> internals,
             OpenReport report, std::vector<uint32_t> checkpoint_covered,
             const OpenOptions& options)
      : lock_(std::move(lock)),
        internals_(std::move(internals)),
        report_(std::move(report)),
        write_checkpoint_(options.write_index_checkpoint),
        checkpoint_stale_containers_(options.checkpoint_stale_containers),
        checkpoint_covered_(std::move(checkpoint_covered)) {}

 private:
  // Whether Close should write a checkpoint (OpenOptions::checkpoint_stale_containers).
  bool CheckpointIsStale() DDS_EXCLUDES(checkpoint_mu_);

  std::unique_ptr<RepoLock> lock_;
  std::unique_ptr<RepoInternals> internals_;
  OpenReport report_;
  const bool write_checkpoint_;
  const size_t checkpoint_stale_containers_;
  OrderedMutex checkpoint_mu_{lock_level::kLeaf, "repo.checkpoint"};
  // The containers the checkpoint on disk covers (increasing ids).
  std::vector<uint32_t> checkpoint_covered_ DDS_GUARDED_BY(checkpoint_mu_);
};

// Reads every container's footer in containers/ and rebuilds `internals`' index (7.6 steps 5-7),
// ignoring any index checkpoint. For tests.
Result<OpenReport> LoadContainers(RepoInternals& internals, size_t threads);

// 7.6 steps 5-7 as Repository::Open runs them: the index checkpoint's entries for the containers
// it covers, if it passes every check, then the footers of every other container (D104).
struct LoadedRepository {
  std::unique_ptr<RepoInternals> internals;
  OpenReport report;
  std::vector<uint32_t> checkpoint_covered;  // containers taken from the checkpoint
};
Result<LoadedRepository> LoadRepository(Fs& fs, const RepoLayout& layout, const RepoConfig& config,
                                        const Clock& clock, const OpenOptions& options);

}  // namespace dds
