#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "dds/common/epoch.h"
#include "dds/hash/fingerprint.h"
#include "dds/store/types.h"

namespace dds {

// One index entry: everything the index knows about a blob, in exactly one 64-byte cache line
// (ARCHITECTURE.md 8), so a probe touches one line.
struct alignas(64) IndexEntry {
  Fingerprint fp;
  uint32_t container_id = 0;
  uint32_t offset = 0;
  uint32_t length = 0;  // as stored; 0 marks an empty slot: a stored blob is never empty
  uint32_t crc32c = 0;
  Epoch last_ref = 0;  // highest epoch that referenced this blob (D20); 0 after open
  GcRun mark = 0;      // last GC run that found it reachable; 0 after open
  BlobType type = BlobType::kData;
  Codec codec = Codec::kNone;  // in what was padding: the entry stays one cache line (D107)

  bool empty() const { return length == 0; }
  Location location() const { return {container_id, offset, length, crc32c, type, codec}; }
  void set_location(const Location& loc) {
    container_id = loc.container_id;
    offset = loc.offset;
    length = loc.length;
    crc32c = loc.crc32c;
    type = loc.type;
    codec = loc.codec;
  }
};
static_assert(sizeof(IndexEntry) == 64, "an index entry must be one cache line");

// A hash table of IndexEntry keyed by fingerprint (D18): open addressing with linear probing over
// a power-of-two array, growth at load factor 0.75, and backward-shift deletion. Not thread-safe:
// ChunkIndex wraps each table in a shard mutex.
//
// Why these choices: fingerprints are already uniformly random, so bytes 8..15 index the array
// directly with no extra hashing. Linear probing over 64-byte entries reads consecutive cache
// lines. Backward-shift deletion moves later entries of a probe chain back into the hole, so no
// tombstones accumulate when GC deletes many entries and lookups never scan dead slots.
class FlatTable {
 public:
  // `initial_capacity` must be a power of two.
  explicit FlatTable(size_t initial_capacity = 16);

  IndexEntry* Find(const Fingerprint& fp);
  const IndexEntry* Find(const Fingerprint& fp) const;

  // Inserts `entry` (which must not be empty) unless an entry with its fingerprint exists.
  // Returns the entry now in the table and whether it was inserted. The pointer is valid until the
  // next Insert, which may grow the table.
  std::pair<IndexEntry*, bool> Insert(const IndexEntry& entry);

  // Removes the entry for `fp`; false if absent.
  bool Erase(const Fingerprint& fp);

  // Grows the table, if needed, so that `n` entries fit without it growing again. Loading a known
  // number of entries (an index checkpoint shard, D104) then rehashes at most once.
  void Reserve(size_t n);

  size_t size() const { return size_; }
  size_t capacity() const { return slots_.size(); }
  size_t MemoryBytes() const { return slots_.size() * sizeof(IndexEntry); }

  void ForEach(const std::function<void(const IndexEntry&)>& fn) const;

 private:
  size_t Home(const Fingerprint& fp) const { return fp.Hash64() & (slots_.size() - 1); }
  // The slot holding `fp`, or the empty slot that ends its probe chain.
  size_t Probe(const Fingerprint& fp) const;
  void Grow();
  void Rehash(size_t capacity);

  std::vector<IndexEntry> slots_;
  size_t size_ = 0;
};

}  // namespace dds
