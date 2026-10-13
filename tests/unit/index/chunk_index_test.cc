#include "dds/index/chunk_index.h"

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dds/common/encoding.h"

namespace dds {
namespace {

Fingerprint Fp(int n) { return FingerprintOf(BlobType::kData, AsBytes(std::to_string(n))); }

SealedEntry Sealed(int n, uint32_t container, uint32_t offset = 32) {
  return {Fp(n),
          {.container_id = container,
           .offset = offset,
           .length = 10,
           .crc32c = 1,
           .type = BlobType::kData}};
}

// last_ref and mark of `fp`, read through ForEachInShard.
IndexEntry Inspect(const ChunkIndex& index, const Fingerprint& fp) {
  IndexEntry found;
  index.ForEachInShard(fp.ShardByte(), [&](const IndexEntry& e) {
    if (e.fp == fp) found = e;
  });
  return found;
}

TEST(ChunkIndexTest, PublishInsertsAbsentEntriesAndCountsDuplicates) {
  ChunkIndex index;
  const std::vector<SealedEntry> first = {Sealed(1, 1), Sealed(2, 1, 42)};
  EXPECT_EQ(index.Publish(first, /*now=*/3), 0u);
  EXPECT_EQ(index.Size(), 2u);
  // Another session sealed entry 2 again, in container 2: the index keeps container 1's copy.
  const std::vector<SealedEntry> second = {Sealed(2, 2), Sealed(3, 2, 42)};
  EXPECT_EQ(index.Publish(second, /*now=*/5), 1u);
  EXPECT_EQ(index.Lookup(Fp(2))->container_id, 1u);
  EXPECT_EQ(index.Lookup(Fp(3))->container_id, 2u);
  EXPECT_EQ(Inspect(index, Fp(1)).last_ref, 3u);
  EXPECT_EQ(Inspect(index, Fp(2)).last_ref, 5u);  // the skipped duplicate still stamps (D50)
}

TEST(ChunkIndexTest, StampsTakeTheMaximum) {
  ChunkIndex index;
  const std::vector<SealedEntry> e = {Sealed(1, 1)};
  index.Publish(e, 2);
  ASSERT_TRUE(index.LookupAndStamp(Fp(1), 9).has_value());
  ASSERT_TRUE(index.LookupAndStamp(Fp(1), 4).has_value());  // a delayed, older stamp
  EXPECT_EQ(Inspect(index, Fp(1)).last_ref, 9u);
  EXPECT_FALSE(index.LookupAndStamp(Fp(2), 10).has_value());
  EXPECT_EQ(index.Lookup(Fp(2)), std::nullopt);
}

TEST(ChunkIndexTest, RebuildKeepsTheHighestContainerInAnyOrder) {
  ChunkIndex ascending;
  EXPECT_FALSE(ascending.RebuildInsert(Sealed(1, 3)));
  EXPECT_TRUE(ascending.RebuildInsert(Sealed(1, 8)));
  EXPECT_EQ(ascending.Lookup(Fp(1))->container_id, 8u);
  ChunkIndex descending;
  EXPECT_FALSE(descending.RebuildInsert(Sealed(1, 8)));
  EXPECT_TRUE(descending.RebuildInsert(Sealed(1, 3)));
  EXPECT_EQ(descending.Lookup(Fp(1))->container_id, 8u);
  const IndexEntry e = Inspect(descending, Fp(1));
  EXPECT_EQ(e.last_ref, 0u);  // rebuilt entries start unstamped and unmarked (6.1)
  EXPECT_EQ(e.mark, 0u);
}

TEST(ChunkIndexTest, MarkLive) {
  ChunkIndex index;
  const std::vector<SealedEntry> e = {Sealed(1, 4)};
  index.Publish(e, 1);
  auto marked = index.MarkLive(Fp(1), 7);
  ASSERT_TRUE(marked.has_value());
  EXPECT_EQ(marked->loc.container_id, 4u);
  EXPECT_TRUE(marked->first);
  EXPECT_EQ(Inspect(index, Fp(1)).mark, 7u);
  EXPECT_FALSE(index.MarkLive(Fp(1), 7)->first);  // already marked in run 7
  EXPECT_TRUE(index.MarkLive(Fp(1), 8)->first);   // a new run
  EXPECT_FALSE(index.MarkLive(Fp(2), 7).has_value());
}

TEST(ChunkIndexTest, RemoveIfDeadFollowsTheRule) {
  ChunkIndex index;
  const std::vector<SealedEntry> e = {Sealed(1, 4), Sealed(2, 4, 42), Sealed(3, 4, 52)};
  index.Publish(e, /*now=*/2);
  const Location here = Sealed(1, 4).loc;
  // Absent, or present elsewhere: nothing to do.
  EXPECT_EQ(index.RemoveIfDead(Fp(9), here, 1, 5), RemoveResult::kNotCanonical);
  EXPECT_EQ(index.RemoveIfDead(Fp(1), Sealed(1, 5).loc, 1, 5), RemoveResult::kNotCanonical);
  // Marked in this run: kept.
  index.MarkLive(Fp(1), 1);
  EXPECT_EQ(index.RemoveIfDead(Fp(1), here, 1, 5), RemoveResult::kKept);
  // Protected by its stamp (last_ref >= E_min): kept.
  index.LookupAndStamp(Fp(2), 6);
  EXPECT_EQ(index.RemoveIfDead(Fp(2), Sealed(2, 4, 42).loc, 1, 5), RemoveResult::kKept);
  // Unmarked and last_ref (2) < E_min (5): removed.
  EXPECT_EQ(index.RemoveIfDead(Fp(3), Sealed(3, 4, 52).loc, 1, 5), RemoveResult::kRemoved);
  EXPECT_EQ(index.Lookup(Fp(3)), std::nullopt);
  // A mark from an earlier run does not protect it in a later one.
  EXPECT_EQ(index.RemoveIfDead(Fp(1), here, 2, 5), RemoveResult::kRemoved);
}

TEST(ChunkIndexTest, RelocateIsCompareAndSwap) {
  ChunkIndex index;
  const std::vector<SealedEntry> e = {Sealed(1, 4)};
  index.Publish(e, 1);
  const Location from = Sealed(1, 4).loc;
  const Location to = Sealed(1, 9, 64).loc;
  EXPECT_FALSE(index.Relocate(Fp(1), to, from));  // not at `to`
  EXPECT_TRUE(index.Relocate(Fp(1), from, to));
  EXPECT_EQ(index.Lookup(Fp(1)), to);
  EXPECT_FALSE(index.Relocate(Fp(1), from, to));  // no longer at `from`
  EXPECT_FALSE(index.Relocate(Fp(2), from, to));  // absent
}

TEST(ChunkIndexTest, SizeAndMemory) {
  ChunkIndex index;
  EXPECT_EQ(index.Size(), 0u);
  const size_t empty_bytes = index.MemoryBytes();
  EXPECT_GT(empty_bytes, 0u);
  std::vector<SealedEntry> many;
  for (int i = 0; i < 10000; ++i) many.push_back(Sealed(i, 1, 32 + 10 * static_cast<uint32_t>(i)));
  index.Publish(many, 1);
  EXPECT_EQ(index.Size(), 10000u);
  EXPECT_GE(index.MemoryBytes(), 10000u * 64u);
}

TEST(ChunkIndexTest, ConcurrentPublishAndLookup) {
  ChunkIndex index;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 5000;
  std::vector<std::thread> threads;
  std::atomic<int> misses{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const int n = t * kPerThread + i;
        const std::vector<SealedEntry> one = {Sealed(n, static_cast<uint32_t>(t + 1))};
        index.Publish(one, static_cast<Epoch>(i + 1));
        if (!index.LookupAndStamp(Fp(n), static_cast<Epoch>(i + 2))) misses.fetch_add(1);
        index.Lookup(Fp((n * 7919) % (kThreads * kPerThread)));  // may or may not be there yet
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(misses.load(), 0);
  EXPECT_EQ(index.Size(), static_cast<size_t>(kThreads * kPerThread));
}

}  // namespace
}  // namespace dds
