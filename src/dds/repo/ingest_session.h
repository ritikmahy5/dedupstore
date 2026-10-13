#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "dds/common/epoch.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/result.h"
#include "dds/common/thread_annotations.h"
#include "dds/hash/fingerprint.h"
#include "dds/snapshot/manifest.h"
#include "dds/store/container_writer.h"

namespace dds {

struct RepoInternals;

// Whether a lookup miss reserves the fingerprint for the caller (ARCHITECTURE.md 4.1, 4.4).
enum class Reserve {
  kOn,   // backup: the missing blob will be Put by this caller, so other workers skip it
  kOff,  // replication HAVE: the sender may never send it, so nothing is reserved (D55)
};

enum class LookupResult { kHit, kMiss };

struct IngestCounters {
  uint64_t blobs_written = 0;
  uint64_t bytes_written = 0;
  uint64_t dedup_hits = 0;
  uint64_t duplicates_published = 0;  // blobs another session published first (D32)
  uint64_t containers_sealed = 0;
};

// One operation that adds blobs and may commit a snapshot: a backup, or an incoming replication
// (SPEC.md section 1). Thread-safe for the pattern backup uses: each of `writer_count` writers is
// owned by exactly one thread (D31), and every thread may call Lookup and Put for its own writer.
//
// Fingerprints this session has reserved or written but not yet published live in a pending map,
// split into 256 shards by fingerprint byte 0, each under its own session shard mutex (D101). A
// writer that fills up is sealed and published, and only then are its fingerprints removed from
// the pending map, so a fingerprint is always findable in the pending map or the index, never in
// neither (ARCHITECTURE.md 4.1). Nothing reaches the index before its container is durable (D11).
// Both guarantees concern one fingerprint at a time, so each operation locks only that
// fingerprint's shard, and no thread holds two shards at once.
class IngestSession {
 public:
  IngestSession(RepoInternals& repo, size_t writer_count);
  ~IngestSession();
  IngestSession(const IngestSession&) = delete;
  IngestSession& operator=(const IngestSession&) = delete;

  // Hit: pending in this session (with kOff, only blobs already received) or in the index (which
  // stamps it). Miss with kOn: the fingerprint is now reserved and the caller must Put it.
  LookupResult Lookup(const Fingerprint& fp, Reserve reserve);

  // Adds a blob to writer `writer` and seals that writer if it is full. Only the thread that owns
  // `writer` may call this for it.
  Status Put(const Fingerprint& fp, BlobType type, std::span<const std::byte> payload,
             size_t writer);

  // Seals and publishes every writer that holds blobs. Call only when no thread is using a writer.
  Status SealAll();

  // ARCHITECTURE.md 7.3: seal and publish everything, then commit the manifest. When OK returns,
  // the snapshot is durable.
  Result<SnapshotId> Commit(std::span<const std::byte> manifest_bytes);

  // The epoch value this session stamps with (6.1), read after the session registered.
  Epoch now() const;
  Epoch start() const { return start_; }

  IngestCounters counters() const;
  size_t writer_count() const { return writers_.size(); }

 private:
  // Seal protocol: seal (I/O, no lock held), publish to the index, mark the container published,
  // and only then drop its fingerprints from the pending map.
  Status SealWriter(size_t writer);

  // One shard of the pending map. alignas(64), as in ChunkIndex: neighboring shards' mutexes stay
  // off one cache line, so workers in different shards do not slow each other down.
  struct alignas(64) PendingShard {
    OrderedMutex mu{lock_level::kSession, "session.shard"};
    // Fingerprint -> received (true: written into a writer; false: reserved by a lookup miss).
    std::unordered_map<Fingerprint, bool, FingerprintHash> pending DDS_GUARDED_BY(mu);
  };
  static constexpr size_t kPendingShards = 256;
  PendingShard& ShardFor(const Fingerprint& fp) { return shards_[fp.ShardByte()]; }

  RepoInternals& repo_;
  const Epoch start_;  // registered with the epoch manager for the session's lifetime
  std::vector<std::unique_ptr<ContainerWriter>> writers_;  // writer i: owned by one thread
  std::array<PendingShard, kPendingShards> shards_;

  // Relaxed: plain statistics, read for reporting after the workers are joined.
  std::atomic<uint64_t> blobs_written_{0};
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> dedup_hits_{0};
  std::atomic<uint64_t> duplicates_{0};
  std::atomic<uint64_t> sealed_{0};
};

}  // namespace dds
