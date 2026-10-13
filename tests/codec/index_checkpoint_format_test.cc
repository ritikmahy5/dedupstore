// Index checkpoint codec (ON_DISK_FORMAT.md section 9; D104; TESTING.md 3.3): round trip, the
// byte layout, every single-bit corruption and truncation, and each structural rule.

#include "dds/repo/index_checkpoint_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/store/container_format.h"
#include "support/test_seed.h"

namespace dds {
namespace {

constexpr uint32_t kMaxData = 65536;

RepoId TestRepoId() {
  RepoId id{};
  for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<uint8_t>(0xA0 + i);
  return id;
}

// A container whose sizes satisfy the identities ON_DISK_FORMAT.md 5.4 requires.
CheckpointContainer Container(uint32_t id, uint32_t entries, uint64_t payload) {
  const uint64_t file = kContainerHeaderSize + payload + 4 + uint64_t{kFooterEntrySize} * entries +
                        kContainerTrailerSize;
  return {id, {payload, file, entries}};
}

// Encodes `entries` (any order) as a checkpoint covering `containers`, as the writer does: each
// shard's entries sorted by fingerprint, then the tables and trailer.
std::vector<std::byte> Build(std::vector<SealedEntry> entries,
                             const std::vector<CheckpointContainer>& containers,
                             uint32_t next_id = 1000) {
  CheckpointTables tables;
  tables.next_container_id = next_id;
  tables.containers = containers;
  std::sort(entries.begin(), entries.end(),
            [](const SealedEntry& a, const SealedEntry& b) { return a.fp < b.fp; });
  std::vector<std::byte> file;
  for (size_t shard = 0; shard < kCheckpointShards; ++shard) {
    std::vector<std::byte> bytes;
    for (const SealedEntry& e : entries) {
      if (e.fp.ShardByte() == shard) AppendCheckpointEntry(e, bytes);
    }
    tables.shards[shard].entry_count = static_cast<uint32_t>(bytes.size() / kCheckpointEntrySize);
    tables.shards[shard].crc32c = Crc32c(bytes);
    tables.entry_count += tables.shards[shard].entry_count;
    file.insert(file.end(), bytes.begin(), bytes.end());
  }
  const std::vector<std::byte> tail = EncodeCheckpointTail(tables, TestRepoId());
  file.insert(file.end(), tail.begin(), tail.end());
  return file;
}

SealedEntry Entry(uint8_t shard, uint8_t tag, uint32_t container, uint32_t offset, uint32_t length,
                  BlobType type = BlobType::kData) {
  SealedEntry e;
  e.fp.bytes.fill(tag);
  e.fp.bytes[0] = shard;
  e.loc = {container, offset, length, 0x12345678u + tag, type};
  return e;
}

// Three containers, entries in several shards, two in one shard, all three blob types.
std::vector<std::byte> Sample() {
  return Build({Entry(0, 1, 3, 32, 100), Entry(7, 2, 3, 132, 50, BlobType::kTree),
                Entry(7, 1, 9, 32, 10, BlobType::kRecipe), Entry(255, 4, 12, 32, 4096)},
               {Container(3, 2, 150), Container(9, 1, 10), Container(12, 1, 4096)});
}

Result<DecodedCheckpoint> Decode(std::span<const std::byte> file) {
  return DecodeIndexCheckpoint(file, "ckpt", TestRepoId(), kMaxData);
}

TEST(IndexCheckpointFormatTest, RoundTripRecoversEveryEntryAndContainer) {
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    std::vector<CheckpointContainer> containers;
    std::map<Fingerprint, SealedEntry> expected;
    for (uint32_t id = 1; id <= 20; id += 1 + static_cast<uint32_t>(rng() % 3)) {
      const uint32_t n = 1 + static_cast<uint32_t>(rng() % 40);
      uint32_t offset = kContainerHeaderSize;
      for (uint32_t i = 0; i < n; ++i) {
        SealedEntry e;
        for (auto& b : e.fp.bytes) b = static_cast<uint8_t>(rng());
        e.loc = {id, offset, 1 + static_cast<uint32_t>(rng() % 5000), static_cast<uint32_t>(rng()),
                 BlobType::kData};
        offset += e.loc.length;
        expected[e.fp] = e;
      }
      containers.push_back(Container(id, n, offset - kContainerHeaderSize));
    }
    std::vector<SealedEntry> entries;
    for (const auto& [fp, e] : expected) entries.push_back(e);
    std::shuffle(entries.begin(), entries.end(), rng);
    auto decoded = Decode(Build(entries, containers, 77));
    ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
    EXPECT_EQ(decoded->tables.next_container_id, 77u);
    ASSERT_EQ(decoded->tables.containers.size(), containers.size());
    for (size_t i = 0; i < containers.size(); ++i) {
      EXPECT_EQ(decoded->tables.containers[i].id, containers[i].id);
      EXPECT_EQ(decoded->tables.containers[i].info.file_bytes, containers[i].info.file_bytes);
    }
    ASSERT_EQ(decoded->entries.size(), expected.size());
    for (const SealedEntry& e : decoded->entries) {
      EXPECT_EQ(e.loc, expected.at(e.fp).loc) << e.fp.ToHex();
    }
  }
}

TEST(IndexCheckpointFormatTest, ByteLayoutMatchesTheFormatDocument) {
  SealedEntry e = Entry(5, 9, 3, 32, 100);
  e.loc.codec = Codec::kZstd;
  const std::vector<std::byte> file = Build({e}, {Container(3, 1, 100)}, 4);
  // 1 entry + 1 container + the shard table + the trailer.
  ASSERT_EQ(file.size(), 50u + 24u + 2048u + 52u);
  // Entry: fp, type, codec, container, offset, stored length, CRC32C.
  EXPECT_TRUE(std::equal(e.fp.bytes.begin(), e.fp.bytes.end(), file.begin(),
                         [](uint8_t a, std::byte b) { return a == static_cast<uint8_t>(b); }));
  EXPECT_EQ(static_cast<uint8_t>(file[32]), 1u);
  EXPECT_EQ(static_cast<uint8_t>(file[33]), 1u);
  EXPECT_EQ(LoadU32(&file[34]), 3u);
  EXPECT_EQ(LoadU32(&file[38]), 32u);
  EXPECT_EQ(LoadU32(&file[42]), 100u);
  EXPECT_EQ(LoadU32(&file[46]), e.loc.crc32c);
  // Container table: id, payload_bytes, file_bytes, entry_count.
  EXPECT_EQ(LoadU32(&file[50]), 3u);
  EXPECT_EQ(LoadU64(&file[54]), 100u);
  EXPECT_EQ(LoadU64(&file[62]), 32u + 100u + 4u + 50u + 32u);
  EXPECT_EQ(LoadU32(&file[70]), 1u);
  // Shard table: shard 5 holds the one entry, with the CRC32C of its 50 bytes.
  const size_t shards = 74;
  EXPECT_EQ(LoadU32(&file[shards + 5 * 8]), 1u);
  EXPECT_EQ(LoadU32(&file[shards + 5 * 8 + 4]), Crc32c(std::span(file).first(50)));
  EXPECT_EQ(LoadU32(&file[shards + 4 * 8]), 0u);
  // Trailer: magic, version, repo_id, next id, counts, tables CRC, trailer CRC.
  const size_t t = file.size() - 52;
  EXPECT_EQ(AsChars(std::span(file).subspan(t, 8)), "DDSIDX01");
  EXPECT_EQ(LoadU32(&file[t + 8]), 2u);
  EXPECT_EQ(static_cast<uint8_t>(file[t + 12]), 0xA0u);
  EXPECT_EQ(LoadU32(&file[t + 28]), 4u);
  EXPECT_EQ(LoadU32(&file[t + 32]), 1u);
  EXPECT_EQ(LoadU64(&file[t + 36]), 1u);
  EXPECT_EQ(LoadU32(&file[t + 44]), Crc32c(std::span(file).subspan(50, 24 + 2048)));
  EXPECT_EQ(LoadU32(&file[t + 48]), Crc32c(std::span(file).subspan(t, 48)));
  auto decoded = Decode(file);
  ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
  EXPECT_EQ(decoded->entries[0].loc.codec, Codec::kZstd);
}

// tests/fixtures/index_checkpoint_sample.bin was written by make_index_checkpoint_sample.py from
// ON_DISK_FORMAT.md section 9 alone, so the document, that encoder, and this one must all agree.
TEST(IndexCheckpointFormatTest, SampleEqualsTheIndependentEncodingFromTheFormatDocument) {
  std::ifstream in(std::string(DDS_FIXTURES_DIR) + "/index_checkpoint_sample.bin",
                   std::ios::binary);
  ASSERT_TRUE(in.good());
  std::vector<std::byte> fixture;
  for (char c; in.get(c);) fixture.push_back(static_cast<std::byte>(c));
  EXPECT_EQ(fixture, Sample());
  EXPECT_TRUE(Decode(fixture).ok());
}

TEST(IndexCheckpointFormatTest, EverySingleBitFlipIsDetected) {
  const std::vector<std::byte> good = Sample();
  ASSERT_TRUE(Decode(good).ok());
  for (size_t i = 0; i < good.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::byte> bad = good;
      bad[i] ^= static_cast<std::byte>(1 << bit);
      EXPECT_FALSE(Decode(bad).ok()) << "byte " << i << " bit " << bit;
    }
  }
}

TEST(IndexCheckpointFormatTest, EveryTruncationIsDetected) {
  const std::vector<std::byte> good = Sample();
  for (size_t n = 0; n < good.size(); ++n) {
    EXPECT_FALSE(Decode(std::span(good).first(n)).ok()) << n;
  }
}

TEST(IndexCheckpointFormatTest, WrongRepositoryAndUnknownVersion) {
  std::vector<std::byte> file = Sample();
  RepoId other = TestRepoId();
  other[0] ^= 1;
  EXPECT_EQ(DecodeIndexCheckpoint(file, "ckpt", other, kMaxData).status().code(),
            StatusCode::kCorruption);
  // Version 1 (before D107) and 3, under a valid trailer CRC, are Incompatible, not Corruption.
  const size_t t = file.size() - kCheckpointTrailerSize;
  StoreU32(&file[t + 8], 1);
  StoreU32(&file[t + 48], Crc32c(std::span(file).subspan(t, 48)));
  EXPECT_EQ(Decode(file).status().code(), StatusCode::kIncompatible);
  StoreU32(&file[t + 8], 3);
  StoreU32(&file[t + 48], Crc32c(std::span(file).subspan(t, 48)));
  EXPECT_EQ(Decode(file).status().code(), StatusCode::kIncompatible);
}

// Each case is encoded with valid CRCs, so only the structural rule can reject it.
TEST(IndexCheckpointFormatTest, StructuralRulesAreEnforced) {
  const auto c3 = Container(3, 2, 150);
  struct Case {
    const char* what;
    std::vector<SealedEntry> entries;
    std::vector<CheckpointContainer> containers;
  };
  const std::vector<Case> cases = {
      {"container not in the table", {Entry(0, 1, 4, 32, 100)}, {c3}},
      {"range past the payloads", {Entry(0, 1, 3, 100, 83)}, {c3}},
      {"offset inside the header", {Entry(0, 1, 3, 31, 10)}, {c3}},
      {"empty blob", {Entry(0, 1, 3, 32, 0)}, {c3}},
      {"data blob above max_size", {Entry(0, 1, 3, 32, kMaxData + 1)}, {Container(3, 1, 70000)}},
      {"containers out of order", {}, {Container(9, 1, 10), Container(3, 1, 10)}},
      {"container id not below next id", {}, {Container(1000, 1, 10)}},
      {"inconsistent container sizes", {}, {{3, {150, 999, 2}}}},
      {"invalid codec",
       {[] {
         SealedEntry e = Entry(0, 1, 3, 32, 100);
         e.loc.codec = static_cast<Codec>(7);
         return e;
       }()},
       {c3}},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(Decode(Build(c.entries, c.containers)).status().code(), StatusCode::kCorruption)
        << c.what;
  }
}

TEST(IndexCheckpointFormatTest, EntriesOutOfOrderOrInTheWrongShardAreCorruption) {
  // Build sorts, so encode these two by hand: shard 7, the larger fingerprint first.
  const auto c3 = Container(3, 2, 150);
  for (bool wrong_shard : {false, true}) {
    CheckpointTables tables;
    tables.next_container_id = 10;
    tables.containers = {c3};
    std::vector<std::byte> bytes;
    AppendCheckpointEntry(Entry(7, 2, 3, 32, 100), bytes);
    AppendCheckpointEntry(Entry(wrong_shard ? 8 : 7, wrong_shard ? 3 : 1, 3, 132, 50), bytes);
    tables.shards[7] = {2, Crc32c(bytes), 0};
    tables.entry_count = 2;
    std::vector<std::byte> file = bytes;
    const auto tail = EncodeCheckpointTail(tables, TestRepoId());
    file.insert(file.end(), tail.begin(), tail.end());
    EXPECT_EQ(Decode(file).status().code(), StatusCode::kCorruption) << wrong_shard;
  }
}

TEST(IndexCheckpointFormatTest, EmptyCheckpointIsValid) {
  auto decoded = Decode(Build({}, {}, 1));
  ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
  EXPECT_TRUE(decoded->entries.empty());
  EXPECT_TRUE(decoded->tables.containers.empty());
}

}  // namespace
}  // namespace dds
