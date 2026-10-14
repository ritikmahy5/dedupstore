// Container codec (ON_DISK_FORMAT.md section 5; TESTING.md 3.3): round trip, golden bytes, and
// every single-bit corruption.

#include "dds/store/container_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/hash/fingerprint.h"
#include "support/random_data.h"
#include "support/test_seed.h"

namespace dds {
namespace {

constexpr uint32_t kMaxData = 65536;

RepoId TestRepoId() {
  RepoId id{};
  for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<uint8_t>(0xA0 + i);
  return id;
}

struct Blob {
  BlobType type;
  std::vector<std::byte> payload;
};

// A container of `blobs`, each stored as its content, in format `version`.
std::vector<std::byte> BuildContainer(uint32_t id, const std::vector<Blob>& blobs,
                                      uint32_t version = kContainerFormatVersion) {
  std::vector<std::byte> file(kContainerHeaderSize);
  std::vector<SealedEntry> entries;
  for (const Blob& b : blobs) {
    SealedEntry e;
    e.fp = FingerprintOf(b.type, b.payload);
    e.loc.container_id = id;
    e.loc.offset = static_cast<uint32_t>(file.size());
    e.loc.length = static_cast<uint32_t>(b.payload.size());
    e.loc.crc32c = Crc32c(b.payload);
    e.loc.type = b.type;
    e.raw_length = e.loc.length;
    entries.push_back(e);
    file.insert(file.end(), b.payload.begin(), b.payload.end());
  }
  const auto header = EncodeContainerHeader(id, TestRepoId(), version);
  std::copy(header.begin(), header.end(), file.begin());
  const std::vector<std::byte> tail = EncodeContainerTail(header, entries);
  file.insert(file.end(), tail.begin(), tail.end());
  return file;
}

std::vector<std::byte> Text(std::string_view s) { return {AsBytes(s).begin(), AsBytes(s).end()}; }

TEST(ContainerFormatTest, FileNames) {
  EXPECT_EQ(ContainerFileName(0x2a), "0000002a.ctr");
  EXPECT_EQ(ContainerFileName(0xffffffff), "ffffffff.ctr");
  EXPECT_EQ(ParseContainerFileName("0000002a.ctr"), 0x2au);
  EXPECT_EQ(ParseContainerFileName("0000002A.ctr"), std::nullopt);
  EXPECT_EQ(ParseContainerFileName("2a.ctr"), std::nullopt);
  EXPECT_EQ(ParseContainerFileName("0000002a.ctr.tmp"), std::nullopt);
  EXPECT_EQ(ParseContainerFileName("0000002a.txt"), std::nullopt);
  EXPECT_EQ(ParseContainerFileName("00000000.ctr"), std::nullopt);  // id 0 is never allocated
}

TEST(ContainerFormatTest, RoundTripRecoversEveryEntry) {
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    std::vector<Blob> blobs;
    for (int i = 0; i < 50; ++i) {
      const auto type = static_cast<BlobType>(1 + rng() % 3);
      blobs.push_back({type, testing::RandomBytes(rng(), 1 + rng() % 5000)});
    }
    blobs.push_back({BlobType::kData, testing::RandomBytes(rng(), kMaxData)});  // max data
    blobs.push_back({BlobType::kData, testing::RandomBytes(rng(), 1)});         // 1 byte
    const auto file = BuildContainer(7, blobs);
    auto meta = DecodeContainerMetadata(file, "c", 7, TestRepoId(), kMaxData);
    ASSERT_TRUE(meta.ok()) << meta.status().ToString();
    EXPECT_EQ(meta->id, 7u);
    EXPECT_EQ(meta->file_size, file.size());
    ASSERT_EQ(meta->entries.size(), blobs.size());
    uint64_t payload = 0;
    for (size_t i = 0; i < blobs.size(); ++i) {
      const SealedEntry& e = meta->entries[i];
      EXPECT_EQ(e.fp, FingerprintOf(blobs[i].type, blobs[i].payload));
      EXPECT_EQ(e.loc.type, blobs[i].type);
      EXPECT_EQ(e.loc.length, blobs[i].payload.size());
      EXPECT_EQ(e.loc.container_id, 7u);
      const auto bytes = std::span(file).subspan(e.loc.offset, e.loc.length);
      EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), blobs[i].payload.begin()));
      EXPECT_EQ(e.loc.crc32c, Crc32c(bytes));
      payload += e.loc.length;
    }
    EXPECT_EQ(meta->payload_bytes, payload);
  }
}

// One container with one 3-byte data blob "abc", written out field by field. It catches any
// accidental change to the byte layout of ON_DISK_FORMAT.md section 5.
TEST(ContainerFormatTest, GoldenBytes) {
  const std::vector<std::byte> payload = Text("abc");
  const auto file = BuildContainer(1, {{BlobType::kData, payload}});
  Writer w;
  // Header (5.1): magic, version 2, container id 1, repo_id.
  w.Bytes(AsBytes("DDSCTR01"));
  w.U32(2);
  w.U32(1);
  const RepoId repo = TestRepoId();
  w.Bytes(std::as_bytes(std::span(repo)));
  // Blob region (5.2).
  w.Bytes(payload);
  // Footer (5.3): entry_count, then fp, type, codec, offset, stored length, raw length, crc.
  const size_t footer_start = w.size();
  w.U32(1);
  const Fingerprint fp = FingerprintOf(BlobType::kData, payload);
  w.Bytes(fp.AsBytes());
  w.U8(1);
  w.U8(0);
  w.U32(32);
  w.U32(3);
  w.U32(3);
  w.U32(Crc32c(payload));
  const auto footer = std::span(w.buffer()).subspan(footer_start);
  const uint32_t footer_crc = Crc32c(footer);
  const uint32_t header_crc = Crc32c(std::span(w.buffer()).first(32));
  // Trailer (5.4): footer_offset 35, footer_length 54, entry_count, CRCs, payload_bytes, magic.
  w.U32(35);
  w.U32(4 + 50);
  w.U32(1);
  w.U32(footer_crc);
  w.U32(header_crc);
  w.U64(3);
  w.Bytes(AsBytes("DDSE"));
  EXPECT_EQ(file, w.buffer());
  EXPECT_EQ(file.size(), 32u + 3 + 54 + 32);
}

// Version 1 (before D107), which existing repositories hold, still decodes: the same blob with a
// 45-byte entry (no codec, no raw length).
TEST(ContainerFormatTest, Version1GoldenBytesStillDecode) {
  const std::vector<std::byte> payload = Text("abc");
  const auto file = BuildContainer(1, {{BlobType::kData, payload}}, /*version=*/1);
  Writer w;
  w.Bytes(AsBytes("DDSCTR01"));
  w.U32(1);
  w.U32(1);
  const RepoId repo = TestRepoId();
  w.Bytes(std::as_bytes(std::span(repo)));
  // Blob region (5.2).
  w.Bytes(payload);
  // Footer (5.3): entry_count, then fp, type, offset, length, crc.
  const size_t footer_start = w.size();
  w.U32(1);
  const Fingerprint fp = FingerprintOf(BlobType::kData, payload);
  w.Bytes(fp.AsBytes());
  w.U8(1);
  w.U32(32);
  w.U32(3);
  w.U32(Crc32c(payload));
  const auto footer = std::span(w.buffer()).subspan(footer_start);
  const uint32_t footer_crc = Crc32c(footer);
  const uint32_t header_crc = Crc32c(std::span(w.buffer()).first(32));
  // Trailer (5.4): footer_offset 35, footer_length 49, entry_count, CRCs, payload_bytes, magic.
  w.U32(35);
  w.U32(4 + 45);
  w.U32(1);
  w.U32(footer_crc);
  w.U32(header_crc);
  w.U64(3);
  w.Bytes(AsBytes("DDSE"));
  EXPECT_EQ(file, w.buffer());
  EXPECT_EQ(file.size(), 32u + 3 + 49 + 32);
  auto meta = DecodeContainerMetadata(file, "c", 1, TestRepoId(), kMaxData);
  ASSERT_TRUE(meta.ok()) << meta.status().ToString();
  EXPECT_EQ(meta->entries[0].loc.codec, Codec::kNone);
  EXPECT_EQ(meta->entries[0].raw_length, 3u);
}

TEST(ContainerFormatTest, EverySingleBitFlipIsDetected) {
  // Metadata flips must fail the decode with Corruption (header, footer, and trailer are covered
  // by CRCs and cross-checks). Payload flips leave the metadata valid but change the payload's
  // CRC, which every blob read checks (ContainerReaderTest covers the read path).
  const std::vector<Blob> blobs = {{BlobType::kData, Text("hello")},
                                   {BlobType::kTree, Text("tree bytes")},
                                   {BlobType::kRecipe, Text("r")}};
  for (uint32_t version = kOldestContainerFormatVersion; version <= kContainerFormatVersion;
       ++version) {
    SCOPED_TRACE(version);
    const auto good = BuildContainer(3, blobs, version);
    auto good_meta = DecodeContainerMetadata(good, "c", 3, TestRepoId(), kMaxData);
    ASSERT_TRUE(good_meta.ok());
    const size_t payload_end = kContainerHeaderSize + good_meta->payload_bytes;
    for (size_t byte = 0; byte < good.size(); ++byte) {
      for (int bit = 0; bit < 8; ++bit) {
        auto flipped = good;
        flipped[byte] ^= static_cast<std::byte>(1 << bit);
        auto meta =
            DecodeContainerMetadata(flipped, "containers/00000003.ctr", 3, TestRepoId(), kMaxData);
        const bool in_payload = byte >= kContainerHeaderSize && byte < payload_end;
        if (in_payload) {
          ASSERT_TRUE(meta.ok()) << "byte " << byte << " bit " << bit;
          bool crc_mismatch = false;
          for (const SealedEntry& e : meta->entries) {
            const auto bytes = std::span(flipped).subspan(e.loc.offset, e.loc.length);
            crc_mismatch |= Crc32c(bytes) != e.loc.crc32c;
          }
          EXPECT_TRUE(crc_mismatch) << "byte " << byte << " bit " << bit;
        } else {
          ASSERT_FALSE(meta.ok()) << "byte " << byte << " bit " << bit;
          EXPECT_EQ(meta.status().code(), StatusCode::kCorruption) << meta.status().ToString();
          EXPECT_NE(meta.status().message().find("containers/00000003.ctr"), std::string::npos)
              << meta.status().message();
        }
      }
    }
  }
}

TEST(ContainerFormatTest, RejectsWrongIdentity) {
  const auto file = BuildContainer(5, {{BlobType::kData, Text("x")}});
  EXPECT_EQ(DecodeContainerMetadata(file, "c", 6, TestRepoId(), kMaxData).status().code(),
            StatusCode::kCorruption);  // id differs from the file name
  RepoId other = TestRepoId();
  other[0] ^= 1;
  EXPECT_EQ(DecodeContainerMetadata(file, "c", 5, other, kMaxData).status().code(),
            StatusCode::kCorruption);  // repo_id differs from config
}

TEST(ContainerFormatTest, UnknownVersionWithValidChecksumsIsIncompatible) {
  auto file = BuildContainer(5, {{BlobType::kData, Text("x")}});
  StoreU32(file.data() + 8, 3);  // version 3
  // Re-seal the header CRC so only the version is "wrong".
  const uint32_t header_crc = Crc32c(std::span(file).first(32));
  StoreU32(file.data() + file.size() - 32 + 16, header_crc);
  EXPECT_EQ(DecodeContainerMetadata(file, "c", 5, TestRepoId(), kMaxData).status().code(),
            StatusCode::kIncompatible);
}

TEST(ContainerFormatTest, RejectsTooSmallAndOversizedEntries) {
  EXPECT_EQ(DecodeContainerMetadata(std::vector<std::byte>(113), "c", 1, TestRepoId(), kMaxData)
                .status()
                .code(),
            StatusCode::kCorruption);
  // A data blob longer than max_size violates the config's limit.
  const auto file = BuildContainer(1, {{BlobType::kData, testing::RandomBytes(1, 70000)}});
  EXPECT_EQ(DecodeContainerMetadata(file, "c", 1, TestRepoId(), kMaxData).status().code(),
            StatusCode::kCorruption);
  // The same blob as a tree is fine: metadata blobs may be up to 64 MiB.
  const auto tree = BuildContainer(1, {{BlobType::kTree, testing::RandomBytes(1, 70000)}});
  EXPECT_TRUE(DecodeContainerMetadata(tree, "c", 1, TestRepoId(), kMaxData).ok());
}

TEST(ContainerFormatTest, DuplicateFingerprintIsCorruption) {
  const std::vector<std::byte> same = Text("twice");
  const auto file = BuildContainer(1, {{BlobType::kData, same}, {BlobType::kData, same}});
  auto meta = DecodeContainerMetadata(file, "c", 1, TestRepoId(), kMaxData);
  EXPECT_EQ(meta.status().code(), StatusCode::kCorruption);
}

// The version 2 fuzz seeds, written by tests/fixtures/make_container_v2_seeds.py from
// ON_DISK_FORMAT.md section 5 alone, must decode, or the fuzz target would start from rejected
// bytes; the zstd one must decode as compressed.
TEST(ContainerFormatTest, Version2SeedsFromTheFormatDocumentDecode) {
  for (const char* name : {"v2-plain", "v2-zstd"}) {
    std::ifstream in(std::string(DDS_FIXTURES_DIR) + "/../../fuzz/corpus/container/" + name,
                     std::ios::binary);
    ASSERT_TRUE(in.good()) << name;
    std::vector<std::byte> file;
    for (char c; in.get(c);) file.push_back(static_cast<std::byte>(c));
    auto meta = DecodeContainerMetadata(file, name, 1, TestRepoId(), kMaxData);
    ASSERT_TRUE(meta.ok()) << name << ": " << meta.status().ToString();
    if (std::string(name) == "v2-zstd") {
      EXPECT_EQ(meta->entries[0].loc.codec, Codec::kZstd);
      EXPECT_EQ(meta->entries[0].raw_length, 4096u);
    }
  }
}

// Each version 2 rule about codecs and lengths, with every CRC valid, so only the rule rejects.
TEST(ContainerFormatTest, CodecRulesAreEnforced) {
  auto build = [](Codec codec, uint8_t codec_byte, uint32_t stored, uint32_t raw) {
    std::vector<std::byte> file(kContainerHeaderSize);
    const std::vector<std::byte> bytes(stored, std::byte{7});
    SealedEntry e;
    e.fp.bytes.fill(9);
    e.loc = {1, kContainerHeaderSize, stored, Crc32c(bytes), BlobType::kData, codec};
    e.raw_length = raw;
    file.insert(file.end(), bytes.begin(), bytes.end());
    const auto header = EncodeContainerHeader(1, TestRepoId());
    std::copy(header.begin(), header.end(), file.begin());
    auto tail = EncodeContainerTail(header, std::vector<SealedEntry>{e});
    tail[4 + 33] = static_cast<std::byte>(codec_byte);  // the entry's codec byte
    StoreU32(tail.data() + tail.size() - 32 + 12, Crc32c(std::span(tail).first(tail.size() - 32)));
    file.insert(file.end(), tail.begin(), tail.end());
    return DecodeContainerMetadata(file, "c", 1, TestRepoId(), kMaxData).status().code();
  };
  EXPECT_EQ(build(Codec::kZstd, 1, 10, 100), StatusCode::kOk);
  EXPECT_EQ(build(Codec::kNone, 0, 10, 10), StatusCode::kOk);
  EXPECT_EQ(build(Codec::kZstd, 2, 10, 100), StatusCode::kCorruption);   // unknown codec
  EXPECT_EQ(build(Codec::kZstd, 1, 100, 100), StatusCode::kCorruption);  // frame not smaller
  EXPECT_EQ(build(Codec::kNone, 0, 10, 11), StatusCode::kCorruption);    // content of another size
  EXPECT_EQ(build(Codec::kZstd, 1, 10, kMaxData + 1), StatusCode::kCorruption);  // above max_size
}

}  // namespace
}  // namespace dds
