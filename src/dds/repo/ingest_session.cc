#include "dds/repo/ingest_session.h"

#include "dds/common/test_hooks.h"
#include "dds/repo/repository.h"

namespace dds {

IngestSession::IngestSession(RepoInternals& repo, size_t writer_count)
    : repo_(repo), start_(repo.epochs.BeginSession()) {
  DDS_CHECK(writer_count >= 1);
  for (size_t i = 0; i < writer_count; ++i) {
    writers_.push_back(std::make_unique<ContainerWriter>(repo_.NewWriter()));
  }
}

// An uncommitted session leaves its unsealed blobs unwritten and its sealed containers published
// but unreferenced: garbage for the next GC (D19).
// Deregisters only now, after the commit or abort is complete (6.1).
IngestSession::~IngestSession() { repo_.epochs.EndSession(start_); }

// Read after BeginSession returned, so now() >= start_ (6.1, stamping).
Epoch IngestSession::now() const { return repo_.epochs.Current(); }

LookupResult IngestSession::Lookup(const Fingerprint& fp, Reserve reserve) {
  // The whole lookup runs under fp's session shard, so two workers looking up the same
  // fingerprint are serialized and only one can miss and reserve it (4.1); workers looking up
  // fingerprints in other shards do not wait (D101).
  PendingShard& shard = ShardFor(fp);
  MutexLock lock(shard.mu);
  auto it = shard.pending.find(fp);
  if (it != shard.pending.end() && (reserve == Reserve::kOn || it->second)) {
    dedup_hits_.fetch_add(1, std::memory_order_relaxed);
    return LookupResult::kHit;
  }
  // The index shard lock nests inside the session shard lock (levels 40 then 60) for one probe.
  if (repo_.index.LookupAndStamp(fp, now()).has_value()) {
    dedup_hits_.fetch_add(1, std::memory_order_relaxed);
    return LookupResult::kHit;
  }
  if (reserve == Reserve::kOn) shard.pending.emplace(fp, false);
  return LookupResult::kMiss;
}

Status IngestSession::Put(const Fingerprint& fp, BlobType type, std::span<const std::byte> payload,
                          size_t writer) {
  DDS_CHECK(writer < writers_.size());
  writers_[writer]->Add(fp, type, payload);
  {
    PendingShard& shard = ShardFor(fp);
    MutexLock lock(shard.mu);
    shard.pending[fp] = true;
  }
  blobs_written_.fetch_add(1, std::memory_order_relaxed);
  bytes_written_.fetch_add(payload.size(), std::memory_order_relaxed);
  if (writers_[writer]->Full()) return SealWriter(writer);
  return Status::Ok();
}

Status IngestSession::SealWriter(size_t writer) {
  if (writers_[writer]->Empty()) return Status::Ok();
  auto sealed = writers_[writer]->Seal();
  DDS_TRY(sealed.status());
  // Durable but not yet findable: other sessions miss these blobs and may store copies (D32).
  DDS_SYNC_POINT("seal.before_publish");
  sealed_.fetch_add(1, std::memory_order_relaxed);
  duplicates_.fetch_add(repo_.PublishSealed(*sealed, now()), std::memory_order_relaxed);
  // Every entry is in the index now, so each may leave the pending map. One shard lock at a time.
  for (const SealedEntry& e : sealed->entries) {
    PendingShard& shard = ShardFor(e.fp);
    MutexLock lock(shard.mu);
    shard.pending.erase(e.fp);
  }
  return Status::Ok();
}

Status IngestSession::SealAll() {
  for (size_t i = 0; i < writers_.size(); ++i) DDS_TRY(SealWriter(i));
  return Status::Ok();
}

Result<SnapshotId> IngestSession::Commit(std::span<const std::byte> manifest_bytes) {
  if (DDS_MUTATION("manifest_before_seal")) {
    // Broken on purpose (TESTING.md 3.6): the commit point comes before its blobs are durable.
    auto id = repo_.snapshots.Commit(manifest_bytes);
    DDS_TRY(id.status());
    DDS_TRY(SealAll());
    return id;
  }
  // 7.3 steps 1-2: everything the manifest can reach is durable and published first.
  DDS_TRY(SealAll());
  // Step 3: the manifest's rename is the commit point.
  return repo_.snapshots.Commit(manifest_bytes);
}

IngestCounters IngestSession::counters() const {
  return {blobs_written_.load(std::memory_order_relaxed),
          bytes_written_.load(std::memory_order_relaxed),
          dedup_hits_.load(std::memory_order_relaxed), duplicates_.load(std::memory_order_relaxed),
          sealed_.load(std::memory_order_relaxed)};
}

}  // namespace dds
