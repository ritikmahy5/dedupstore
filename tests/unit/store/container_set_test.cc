#include "dds/store/container_set.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "dds/common/crc32c.h"
#include "dds/fs/mem_fs.h"
#include "dds/store/container_format.h"
#include "dds/store/container_reader.h"
#include "support/counting_fs.h"
#include "support/random_data.h"

namespace dds {
namespace {

RepoId TestRepoId() {
  RepoId id{};
  id.fill(0x17);
  return id;
}

class ContainerSetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(mem_.CreateDir("/r").ok());
    ASSERT_TRUE(mem_.CreateDir("/r/containers").ok());
    ASSERT_TRUE(mem_.CreateDir("/r/tmp").ok());
  }

  // Seals one container holding one blob and returns the blob's location.
  Location SealOne(ContainerSet& set, uint64_t seed, std::vector<std::byte>* payload_out) {
    ContainerWriter w(counting_, "/r/containers", "/r/tmp", TestRepoId(), set, 1 << 20);
    *payload_out = testing::RandomBytes(seed, 100 + seed);
    w.Add(FingerprintOf(BlobType::kData, *payload_out), BlobType::kData, *payload_out);
    auto sealed = w.Seal();
    EXPECT_TRUE(sealed.ok());
    set.MarkPublished(sealed->id, {sealed->payload_bytes, sealed->file_bytes, 1});
    return sealed->entries[0].loc;
  }

  MemFs mem_;
  testing::CountingFs counting_{mem_};
};

TEST_F(ContainerSetTest, AllocatesIncreasingIds) {
  ContainerSet set(mem_, "/r/containers");
  EXPECT_EQ(set.AllocateId(), 1u);
  EXPECT_EQ(set.AllocateId(), 2u);
  set.SetNextId(100);
  EXPECT_EQ(set.NextId(), 100u);
  EXPECT_EQ(set.AllocateId(), 100u);
  EXPECT_EQ(set.PathFor(0x2a), "/r/containers/0000002a.ctr");
}

TEST_F(ContainerSetTest, PublishedSetAndTotals) {
  ContainerSet set(mem_, "/r/containers");
  EXPECT_TRUE(set.PublishedNotRetired().empty());
  set.MarkPublished(3, {.payload_bytes = 100, .file_bytes = 200, .entry_count = 2});
  set.MarkPublished(1, {.payload_bytes = 10, .file_bytes = 20, .entry_count = 1});
  EXPECT_EQ(set.PublishedNotRetired(), (std::vector<uint32_t>{1, 3}));
  ASSERT_TRUE(set.Info(3).has_value());
  EXPECT_EQ(set.Info(3)->entry_count, 2u);
  EXPECT_FALSE(set.Info(2).has_value());
  const auto totals = set.PublishedTotals();
  EXPECT_EQ(totals.containers, 2u);
  EXPECT_EQ(totals.payload_bytes, 110u);
  EXPECT_EQ(totals.file_bytes, 220u);
  EXPECT_EQ(totals.entries, 3u);
}

TEST_F(ContainerSetTest, ReadVerifiesCrcAndNamesTheFile) {
  ContainerSet set(counting_, "/r/containers");
  std::vector<std::byte> payload;
  const Location loc = SealOne(set, 1, &payload);
  auto bytes = set.Read(loc);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  EXPECT_EQ(*bytes, payload);
  Location wrong = loc;
  wrong.crc32c ^= 1;  // what a flipped payload bit looks like to a reader
  auto bad = set.Read(wrong);
  EXPECT_EQ(bad.status().code(), StatusCode::kCorruption);
  EXPECT_NE(bad.status().message().find("00000001.ctr"), std::string::npos);
  EXPECT_NE(bad.status().message().find("offset 32"), std::string::npos);
}

TEST_F(ContainerSetTest, HandleCacheReusesAndEvicts) {
  ContainerSet set(counting_, "/r/containers", /*handle_cache_capacity=*/2);
  std::vector<Location> locs;
  std::vector<std::byte> payload;
  for (uint64_t seed = 1; seed <= 3; ++seed) locs.push_back(SealOne(set, seed, &payload));
  const int base = counting_.open_reads();
  ASSERT_TRUE(set.Read(locs[0]).ok());
  ASSERT_TRUE(set.Read(locs[0]).ok());
  EXPECT_EQ(counting_.open_reads(), base + 1);  // second read used the cached handle
  ASSERT_TRUE(set.Read(locs[1]).ok());
  ASSERT_TRUE(set.Read(locs[2]).ok());  // evicts container 1, the least recently used
  EXPECT_EQ(counting_.open_reads(), base + 3);
  ASSERT_TRUE(set.Read(locs[0]).ok());  // reopened
  EXPECT_EQ(counting_.open_reads(), base + 4);
}

TEST_F(ContainerSetTest, ConcurrentReads) {
  ContainerSet set(counting_, "/r/containers", /*handle_cache_capacity=*/3);
  std::vector<Location> locs;
  std::vector<std::vector<std::byte>> payloads(8);
  for (uint64_t i = 0; i < 8; ++i) locs.push_back(SealOne(set, i + 1, &payloads[i]));
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        const size_t k = static_cast<size_t>((t + i) % 8);
        auto bytes = set.Read(locs[k]);
        ASSERT_TRUE(bytes.ok());
        ASSERT_EQ(*bytes, payloads[k]);
      }
    });
  }
  for (auto& th : threads) th.join();
}

TEST_F(ContainerSetTest, ReadOfMissingContainerIsAnError) {
  ContainerSet set(mem_, "/r/containers");
  Location loc{.container_id = 9, .offset = 32, .length = 1, .crc32c = 0, .type = BlobType::kData};
  EXPECT_EQ(set.Read(loc).status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace dds
