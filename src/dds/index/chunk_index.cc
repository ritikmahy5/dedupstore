#include "dds/index/chunk_index.h"

#include <algorithm>

#include "dds/common/test_hooks.h"

namespace dds {

void ChunkIndex::Stamp(IndexEntry& e, Epoch now) {
  // Max, not assignment: a stamp computed earlier but applied later must not lower last_ref (D50).
  e.last_ref = DDS_MUTATION("stamp_assign") ? now : std::max(e.last_ref, now);
}

bool IsProtected(const IndexEntry& e, Epoch e_min) {
  return !DDS_MUTATION("no_epoch_protection") && e.last_ref >= e_min;
}

std::optional<Location> ChunkIndex::LookupAndStamp(const Fingerprint& fp, Epoch now) {
  Shard& shard = ShardFor(fp);
  MutexLock lock(shard.mu);
  IndexEntry* e = shard.table.Find(fp);
  if (e == nullptr) return std::nullopt;
  Stamp(*e, now);
  return e->location();
}

std::optional<Location> ChunkIndex::Lookup(const Fingerprint& fp) const {
  const Shard& shard = ShardFor(fp);
  MutexLock lock(shard.mu);
  const IndexEntry* e = shard.table.Find(fp);
  if (e == nullptr) return std::nullopt;
  return e->location();
}

size_t ChunkIndex::Publish(std::span<const SealedEntry> entries, Epoch now) {
  size_t duplicates = 0;
  for (const SealedEntry& s : entries) {
    Shard& shard = ShardFor(s.fp);
    MutexLock lock(shard.mu);
    IndexEntry fresh;
    fresh.fp = s.fp;
    fresh.set_location(s.loc);
    fresh.last_ref = now;
    auto [e, inserted] = shard.table.Insert(fresh);
    if (!inserted) {
      // Another session published this blob first; its copy stays canonical and this one is a
      // duplicate (D32). The stamp still counts: this session references the blob too (D50).
      Stamp(*e, now);
      ++duplicates;
    }
  }
  return duplicates;
}

bool ChunkIndex::RebuildInsert(const SealedEntry& entry) {
  Shard& shard = ShardFor(entry.fp);
  MutexLock lock(shard.mu);
  IndexEntry fresh;
  fresh.fp = entry.fp;
  fresh.set_location(entry.loc);
  auto [e, inserted] = shard.table.Insert(fresh);
  if (inserted) return false;
  // D12: the higher container id wins, which makes a GC copy-forward win after a crash.
  if (entry.loc.container_id > e->container_id) e->set_location(entry.loc);
  return true;
}

void ChunkIndex::Reserve(size_t shard_index, size_t n) {
  Shard& shard = shards_[shard_index];
  MutexLock lock(shard.mu);
  shard.table.Reserve(n);
}

std::optional<MarkResult> ChunkIndex::MarkLive(const Fingerprint& fp, GcRun run) {
  Shard& shard = ShardFor(fp);
  MutexLock lock(shard.mu);
  IndexEntry* e = shard.table.Find(fp);
  if (e == nullptr) return std::nullopt;
  const bool first = e->mark != run;
  e->mark = run;
  return MarkResult{e->location(), first};
}

RemoveResult ChunkIndex::RemoveIfDead(const Fingerprint& fp, const Location& here, GcRun run,
                                      Epoch e_min) {
  Shard& shard = ShardFor(fp);
  MutexLock lock(shard.mu);
  IndexEntry* e = shard.table.Find(fp);
  if (e == nullptr || e->location() != here) return RemoveResult::kNotCanonical;
  // ARCHITECTURE.md 6.1: remove only if unmarked in this run and not stamped since E_min.
  if (e->mark == run || IsProtected(*e, e_min)) return RemoveResult::kKept;
  shard.table.Erase(fp);
  return RemoveResult::kRemoved;
}

bool ChunkIndex::Relocate(const Fingerprint& fp, const Location& from, const Location& to) {
  Shard& shard = ShardFor(fp);
  MutexLock lock(shard.mu);
  IndexEntry* e = shard.table.Find(fp);
  if (e == nullptr || e->location() != from) return false;
  e->set_location(to);
  return true;
}

void ChunkIndex::ForEachInShard(size_t shard_index,
                                const std::function<void(const IndexEntry&)>& fn) const {
  const Shard& shard = shards_[shard_index];
  MutexLock lock(shard.mu);
  shard.table.ForEach(fn);
}

size_t ChunkIndex::Size() const {
  size_t total = 0;
  for (const Shard& shard : shards_) {
    MutexLock lock(shard.mu);
    total += shard.table.size();
  }
  return total;
}

size_t ChunkIndex::MemoryBytes() const {
  size_t total = 0;
  for (const Shard& shard : shards_) {
    MutexLock lock(shard.mu);
    total += shard.table.MemoryBytes();
  }
  return total;
}

}  // namespace dds
