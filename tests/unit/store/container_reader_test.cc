#include "dds/store/container_reader.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/fs/mem_fs.h"
#include "dds/store/container_format.h"
#include "dds/store/container_set.h"
#include "dds/store/container_writer.h"
#include "support/file_damage.h"
#include "support/random_data.h"

namespace dds {
namespace {

RepoId TestRepoId() {
  RepoId id{};
  id.fill(0x33);
  return id;
}

using testing::FlipBit;

class ContainerReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fs_.CreateDir("/r").ok());
    ASSERT_TRUE(fs_.CreateDir("/r/containers").ok());
    ASSERT_TRUE(fs_.CreateDir("/r/tmp").ok());
    ContainerWriter w(fs_, "/r/containers", "/r/tmp", TestRepoId(), set_, 1 << 20);
    for (uint64_t seed = 1; seed <= 3; ++seed) {
      payloads_.push_back(testing::RandomBytes(seed, 50 * seed));
      w.Add(FingerprintOf(BlobType::kData, payloads_.back()), BlobType::kData, payloads_.back());
    }
    auto sealed = w.Seal();
    ASSERT_TRUE(sealed.ok());
    sealed_ = *sealed;
    path_ = "/r/containers/" + ContainerFileName(sealed_.id);
  }

  MemFs fs_;
  ContainerSet set_{fs_, "/r/containers"};
  std::vector<std::vector<std::byte>> payloads_;
  SealedContainer sealed_;
  std::string path_;
};

TEST_F(ContainerReaderTest, ReadsBlobsBack) {
  auto file = fs_.OpenRead(path_);
  ASSERT_TRUE(file.ok());
  for (size_t i = 0; i < payloads_.size(); ++i) {
    auto bytes = ReadBlob(**file, path_, sealed_.entries[i].loc, kMaxChunkSizeLimit);
    ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
    EXPECT_EQ(*bytes, payloads_[i]);
  }
}

TEST_F(ContainerReaderTest, EveryBitFlipIsCaughtByOpenOrByRead) {
  // Flipping any single bit of a small container makes open (metadata validation) or
  // read (payload CRC) report Corruption naming the file, and never crashes.
  const uint64_t size = sealed_.file_bytes;
  for (size_t byte = 0; byte < size; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      FlipBit(fs_, path_, byte, bit);
      auto meta = ReadContainerMetadata(fs_, path_, sealed_.id, TestRepoId(), 65536);
      Status found;
      if (!meta.ok()) {
        found = meta.status();
      } else {
        auto file = fs_.OpenRead(path_);
        ASSERT_TRUE(file.ok());
        for (const SealedEntry& e : meta->entries) {
          auto bytes = ReadBlob(**file, path_, e.loc, kMaxChunkSizeLimit);
          if (!bytes.ok()) found = bytes.status();
        }
      }
      ASSERT_EQ(found.code(), StatusCode::kCorruption) << "byte " << byte << " bit " << bit;
      EXPECT_NE(found.message().find(path_), std::string::npos) << found.message();
      FlipBit(fs_, path_, byte, bit);  // restore
    }
  }
}

TEST_F(ContainerReaderTest, TruncatedFileIsCorruption) {
  auto data = ReadWholeFile(fs_, path_);
  ASSERT_TRUE(data.ok());
  ASSERT_TRUE(fs_.Remove(path_).ok());
  auto f = fs_.CreateNew(path_);
  ASSERT_TRUE(f.ok());
  ASSERT_TRUE((*f)->Append(std::span(*data).first(data->size() - 1)).ok());
  ASSERT_TRUE((*f)->Close().ok());
  auto meta = ReadContainerMetadata(fs_, path_, sealed_.id, TestRepoId(), 65536);
  EXPECT_EQ(meta.status().code(), StatusCode::kCorruption);
}

TEST_F(ContainerReaderTest, MissingFileIsNotFound) {
  auto meta = ReadContainerMetadata(fs_, "/r/containers/0000ffff.ctr", 0xffff, TestRepoId(), 65536);
  EXPECT_EQ(meta.status().code(), StatusCode::kNotFound);
}

// D107: a compressed blob whose frame is damaged but whose CRC was recomputed to match (what a
// buggy writer, not bit rot, could produce) reads back as Corruption or as some content, never a
// crash.
TEST(ContainerReaderCompressionTest, ADamagedFrameIsCorruption) {
  ZstdCompressor zstd(3);
  const std::vector<std::byte> content(5000, std::byte{'z'});
  std::vector<std::byte> frame;
  ASSERT_TRUE(zstd.CompressIfSmaller(content, frame));
  MemFs fs;
  ASSERT_TRUE(fs.CreateDir("/c").ok());
  auto write = [&](const std::vector<std::byte>& stored) {
    (void)fs.Remove("/c/blob");
    auto f = fs.CreateNew("/c/blob");
    ASSERT_TRUE(f.ok() && (*f)->Append(stored).ok() && (*f)->Close().ok());
  };
  auto read = [&](const std::vector<std::byte>& stored, uint32_t limit) {
    auto file = fs.OpenRead("/c/blob");
    const Location loc{.container_id = 1,
                       .offset = 0,
                       .length = static_cast<uint32_t>(stored.size()),
                       .crc32c = Crc32c(stored),
                       .type = BlobType::kData,
                       .codec = Codec::kZstd};
    return ReadBlob(**file, "/c/blob", loc, limit);
  };
  write(frame);
  auto good = read(frame, 65536);
  ASSERT_TRUE(good.ok()) << good.status().ToString();
  EXPECT_EQ(*good, content);
  EXPECT_EQ(read(frame, 4999).status().code(), StatusCode::kCorruption);  // above the limit
  // Every single-bit flip: an error is Corruption, never anything else and never a crash (the asan
  // preset checks the bounds). A flip zstd's own checks miss decodes to the same content (a field
  // that does not affect the output) or to other bytes, which the BLAKE3 check of restore --verify
  // and verify --deep finds (D33).
  size_t caught = 0;
  for (size_t i = 0; i < frame.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::byte> bad = frame;
      bad[i] ^= static_cast<std::byte>(1 << bit);
      write(bad);
      auto r = read(bad, 65536);
      if (!r.ok()) {
        EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().ToString();
        ++caught;
      }
    }
  }
  EXPECT_GT(caught, frame.size() * 4);  // most flips fail the decode outright
}

// The fuzz seeds are frames from the zstd command-line tool, one with zstd's content checksum: an
// independent encoder whose frames must decode (D107).
TEST(ContainerReaderCompressionTest, FramesFromTheZstdToolDecode) {
  for (const char* name : {"text", "zeros-checksum"}) {
    std::ifstream in(std::string(DDS_FIXTURES_DIR) + "/../../fuzz/corpus/decompress/" + name,
                     std::ios::binary);
    ASSERT_TRUE(in.good()) << name;
    std::vector<std::byte> frame;
    for (char c; in.get(c);) frame.push_back(static_cast<std::byte>(c));
    auto content = DecompressZstd(frame, 65536);
    ASSERT_TRUE(content.ok()) << name << ": " << content.status().ToString();
    EXPECT_EQ(content->size(), std::string(name) == "text" ? 35u * 120 : 8192u) << name;
  }
}

}  // namespace
}  // namespace dds
