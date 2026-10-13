#include "dds/index/flat_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "dds/common/encoding.h"

namespace dds {
namespace {

// A fingerprint whose slot hash (bytes 8..15) is `hash` and whose other bytes are `tag`, so tests
// can place entries at chosen home slots.
Fingerprint WithHash(uint64_t hash, uint8_t tag) {
  Fingerprint fp;
  fp.bytes.fill(tag);
  for (int i = 0; i < 8; ++i)
    fp.bytes[8 + static_cast<size_t>(i)] = static_cast<uint8_t>(hash >> (8 * i));
  return fp;
}

IndexEntry Entry(const Fingerprint& fp, uint32_t container) {
  IndexEntry e;
  e.fp = fp;
  e.container_id = container;
  e.offset = 32;
  e.length = 100;
  return e;
}

TEST(FlatTableTest, EntryIsOneCacheLine) {
  EXPECT_EQ(sizeof(IndexEntry), 64u);
  EXPECT_EQ(alignof(IndexEntry), 64u);
}

TEST(FlatTableTest, InsertFindErase) {
  FlatTable t;
  const Fingerprint a = WithHash(1, 0xA);
  EXPECT_EQ(t.Find(a), nullptr);
  auto [entry, inserted] = t.Insert(Entry(a, 7));
  EXPECT_TRUE(inserted);
  EXPECT_EQ(entry->container_id, 7u);
  ASSERT_NE(t.Find(a), nullptr);
  EXPECT_EQ(t.size(), 1u);
  EXPECT_TRUE(t.Erase(a));
  EXPECT_FALSE(t.Erase(a));
  EXPECT_EQ(t.Find(a), nullptr);
  EXPECT_EQ(t.size(), 0u);
}

TEST(FlatTableTest, InsertOfExistingKeyKeepsTheOldEntry) {
  FlatTable t;
  const Fingerprint a = WithHash(5, 0xA);
  ASSERT_TRUE(t.Insert(Entry(a, 1)).second);
  auto [entry, inserted] = t.Insert(Entry(a, 2));
  EXPECT_FALSE(inserted);
  EXPECT_EQ(entry->container_id, 1u);
  EXPECT_EQ(t.size(), 1u);
}

TEST(FlatTableTest, GrowsAtLoadFactorThreeQuarters) {
  FlatTable t(16);
  for (uint8_t i = 0; i < 12; ++i) ASSERT_TRUE(t.Insert(Entry(WithHash(i * 977u, i), 1)).second);
  EXPECT_EQ(t.capacity(), 16u);  // 12 / 16 = 0.75
  ASSERT_TRUE(t.Insert(Entry(WithHash(99999, 200), 1)).second);
  EXPECT_EQ(t.capacity(), 32u);
  EXPECT_EQ(t.MemoryBytes(), 32u * 64u);
  for (uint8_t i = 0; i < 12; ++i) EXPECT_NE(t.Find(WithHash(i * 977u, i)), nullptr) << int{i};
}

TEST(FlatTableTest, ReserveMakesRoomForExactlyThatManyEntries) {
  FlatTable t(16);
  ASSERT_TRUE(t.Insert(Entry(WithHash(5, 1), 1)).second);
  t.Reserve(13);  // 13 > 0.75 x 16, so 32 slots; then 13 inserts must not grow it again
  EXPECT_EQ(t.capacity(), 32u);
  EXPECT_NE(t.Find(WithHash(5, 1)), nullptr);  // the rehash kept the entry
  for (uint8_t i = 2; i <= 13; ++i) ASSERT_TRUE(t.Insert(Entry(WithHash(i * 977u, i), 1)).second);
  EXPECT_EQ(t.capacity(), 32u);
  t.Reserve(4);  // never shrinks
  EXPECT_EQ(t.capacity(), 32u);
}

TEST(FlatTableTest, ProbingWrapsAroundTheEndOfTheArray) {
  FlatTable t(16);
  // Three entries whose home is the last slot: they occupy slots 15, 0 and 1.
  const Fingerprint a = WithHash(15, 1);
  const Fingerprint b = WithHash(15 + 16, 2);
  const Fingerprint c = WithHash(15 + 32, 3);
  ASSERT_TRUE(t.Insert(Entry(a, 1)).second);
  ASSERT_TRUE(t.Insert(Entry(b, 2)).second);
  ASSERT_TRUE(t.Insert(Entry(c, 3)).second);
  EXPECT_EQ(t.Find(c)->container_id, 3u);
  // Deleting the head of the chain shifts b and c back; both stay reachable.
  ASSERT_TRUE(t.Erase(a));
  EXPECT_EQ(t.Find(b)->container_id, 2u);
  EXPECT_EQ(t.Find(c)->container_id, 3u);
  ASSERT_TRUE(t.Erase(b));
  EXPECT_EQ(t.Find(c)->container_id, 3u);
}

TEST(FlatTableTest, BackwardShiftDoesNotMoveEntriesBeforeTheirHome) {
  FlatTable t(16);
  // Chain: x (home 3) at 3, y (home 3) at 4, z (home 5) at 5, w (home 3) at 6.
  const Fingerprint x = WithHash(3, 1);
  const Fingerprint y = WithHash(3 + 16, 2);
  const Fingerprint z = WithHash(5, 3);
  const Fingerprint w = WithHash(3 + 32, 4);
  for (const Fingerprint& fp : {x, y, z, w}) ASSERT_TRUE(t.Insert(Entry(fp, 1)).second);
  // Erasing y leaves a hole at 4. z's home is 5, so it must not move to 4; w (home 3) moves into
  // the hole instead. Every entry remains findable and nothing else is lost.
  ASSERT_TRUE(t.Erase(y));
  EXPECT_NE(t.Find(x), nullptr);
  EXPECT_NE(t.Find(z), nullptr);
  EXPECT_NE(t.Find(w), nullptr);
  EXPECT_EQ(t.Find(y), nullptr);
  EXPECT_EQ(t.size(), 3u);
  std::set<uint8_t> tags;
  t.ForEach([&](const IndexEntry& e) { tags.insert(e.fp.bytes[0]); });
  EXPECT_EQ(tags, (std::set<uint8_t>{1, 3, 4}));
}

TEST(FlatTableTest, LocationRoundTrip) {
  IndexEntry e;
  const Location loc{
      .container_id = 9, .offset = 1234, .length = 77, .crc32c = 0xabcd, .type = BlobType::kRecipe};
  e.set_location(loc);
  EXPECT_EQ(e.location(), loc);
  EXPECT_FALSE(e.empty());
}

}  // namespace
}  // namespace dds
