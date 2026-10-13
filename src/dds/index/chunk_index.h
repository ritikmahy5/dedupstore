#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>

#include "dds/common/epoch.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/thread_annotations.h"
#include "dds/index/flat_table.h"
#include "dds/store/types.h"

namespace dds {

// What RemoveIfDead found (ARCHITECTURE.md 4.3 step 4).
enum class RemoveResult {
  kRemoved,       // canonical in the given container and dead: it has left the index
  kKept,          // canonical there, but marked in this run or protected by its stamp: copy it
  kNotCanonical,  // the index points elsewhere (a duplicate copy) or not at all
};

// What MarkLive found: the entry's location, and whether this call marked it (false if it was
// already marked in this run, so the marker need not walk below it again).
struct MarkResult {
  Location loc;
  bool first = false;
};

// Whether a stamp protects `e` from this GC run: stamped at or after E_min (ARCHITECTURE.md 6.1).
// The planner and RemoveIfDead must agree, so both use this.
bool IsProtected(const IndexEntry& e, Epoch e_min);

// The in-memory map from fingerprint to location, derived from container footers on open (D10).
// Thread-safe: 256 shards, chosen by fingerprint byte 0, each a FlatTable under its own mutex
// (D17). Every method locks exactly one shard for one short probe and does no I/O under it.
//
// Besides the location, each entry carries the epoch stamp `last_ref` and the GC mark that make
// concurrent GC safe (ARCHITECTURE.md 6). Stamps only ever increase: they take the max, so a
// delayed stamp with an older epoch cannot lower `last_ref` (D50).
class ChunkIndex {
 public:
  static constexpr size_t kShardCount = 256;

  // On a hit, stamps the entry with `now` and returns its location. `now` must be read after the
  // caller's session or read guard registered (6.1).
  std::optional<Location> LookupAndStamp(const Fingerprint& fp, Epoch now);
  // Location only, no stamp: for readers that are not deduplicating against the entry.
  std::optional<Location> Lookup(const Fingerprint& fp) const;

  // Inserts each sealed entry that is absent, stamped `now`. An entry already present (another
  // session stored the same blob first) keeps its location and is stamped instead. Returns how
  // many were skipped as duplicates (D32).
  size_t Publish(std::span<const SealedEntry> entries, Epoch now);

  // Open's insert: absent -> insert with stamp and mark 0; present -> the copy in the higher
  // container id wins (D12). Returns true if `entry.fp` was already present, i.e. one of the two
  // copies is a duplicate. Gives the same result in any order, so footers may load in parallel.
  bool RebuildInsert(const SealedEntry& entry);

  // Lets shard `shard` hold `n` entries without growing (FlatTable::Reserve).
  void Reserve(size_t shard, size_t n);

  // GC mark phase: sets the entry's mark to `run`. Nullopt if `fp` is not in the index.
  std::optional<MarkResult> MarkLive(const Fingerprint& fp, GcRun run);

  // GC decision for the copy of `fp` at `here`, evaluated under the shard lock so it is ordered
  // with any concurrent stamp (6.2 item 4).
  RemoveResult RemoveIfDead(const Fingerprint& fp, const Location& here, GcRun run, Epoch e_min);

  // Moves the entry from `from` to `to` if it is still at `from` (compare-and-swap). False if it
  // is absent or elsewhere.
  bool Relocate(const Fingerprint& fp, const Location& from, const Location& to);

  // Calls `fn` for every entry of one shard, with that shard locked. `fn` must not call back into
  // the index.
  void ForEachInShard(size_t shard, const std::function<void(const IndexEntry&)>& fn) const;

  size_t Size() const;
  size_t MemoryBytes() const;

 private:
  // alignas(64): keeps neighboring shards' mutexes off one cache line, so threads working in
  // different shards do not slow each other down through false sharing.
  struct alignas(64) Shard {
    mutable OrderedMutex mu{lock_level::kShard, "index.shard"};
    FlatTable table DDS_GUARDED_BY(mu);
  };
  // last_ref = max(last_ref, now), under the entry's shard lock (D50).
  static void Stamp(IndexEntry& e, Epoch now);
  Shard& ShardFor(const Fingerprint& fp) { return shards_[fp.ShardByte()]; }
  const Shard& ShardFor(const Fingerprint& fp) const { return shards_[fp.ShardByte()]; }

  std::array<Shard, kShardCount> shards_;
};

}  // namespace dds
