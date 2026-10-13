#include "dds/store/container_writer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/fs/mem_fs.h"
#include "dds/store/container_format.h"
#include "dds/store/container_reader.h"
#include "dds/store/container_set.h"
#include "support/random_data.h"

namespace dds {
namespace {

RepoId TestRepoId() {
  RepoId id{};
  id.fill(0x42);
  return id;
}

class ContainerWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fs_.CreateDir("/r").ok());
    ASSERT_TRUE(fs_.CreateDir("/r/containers").ok());
    ASSERT_TRUE(fs_.CreateDir("/r/tmp").ok());
  }
  ContainerWriter MakeWriter(uint64_t target) {
    return ContainerWriter(fs_, "/r/containers", "/r/tmp", TestRepoId(), set_, target);
  }
  std::vector<std::byte> Payload(uint64_t seed, size_t size) {
    return testing::RandomBytes(seed, size);
  }

  MemFs fs_;
  ContainerSet set_{fs_, "/r/containers"};
};

TEST_F(ContainerWriterTest, SealWritesADurableValidContainer) {
  ContainerWriter w = MakeWriter(1 << 20);
  EXPECT_TRUE(w.Empty());
  const auto a = Payload(1, 100);
  const auto b = Payload(2, 5000);
  w.Add(FingerprintOf(BlobType::kData, a), BlobType::kData, a);
  w.Add(FingerprintOf(BlobType::kTree, b), BlobType::kTree, b);
  EXPECT_FALSE(w.Empty());
  auto sealed = w.Seal();
  ASSERT_TRUE(sealed.ok()) << sealed.status().ToString();
  EXPECT_EQ(sealed->id, 1u);
  EXPECT_EQ(sealed->payload_bytes, 5100u);
  ASSERT_EQ(sealed->entries.size(), 2u);
  EXPECT_EQ(sealed->entries[0].loc.offset, 32u);
  EXPECT_EQ(sealed->entries[1].loc.offset, 132u);
  EXPECT_EQ(sealed->entries[1].loc.crc32c, Crc32c(b));
  EXPECT_TRUE(w.Empty());

  const std::string path = "/r/containers/" + ContainerFileName(1);
  auto meta = ReadContainerMetadata(fs_, path, 1, TestRepoId(), 65536);
  ASSERT_TRUE(meta.ok()) << meta.status().ToString();
  EXPECT_EQ(meta->file_size, sealed->file_bytes);
  ASSERT_EQ(meta->entries.size(), 2u);
  EXPECT_EQ(meta->entries[0].fp, sealed->entries[0].fp);
  EXPECT_EQ(meta->entries[1].loc, sealed->entries[1].loc);
  EXPECT_TRUE(fs_.ListDir("/r/tmp")->empty());  // the temporary file was renamed away
}

TEST_F(ContainerWriterTest, IdsAreAllocatedAtFirstAddAndIncrease) {
  ContainerWriter idle = MakeWriter(1 << 20);  // never used: allocates nothing
  ContainerWriter w = MakeWriter(1 << 20);
  for (uint32_t expected = 1; expected <= 3; ++expected) {
    const auto p = Payload(expected, 10);
    w.Add(FingerprintOf(BlobType::kData, p), BlobType::kData, p);
    auto sealed = w.Seal();
    ASSERT_TRUE(sealed.ok());
    EXPECT_EQ(sealed->id, expected);
  }
  EXPECT_TRUE(idle.Empty());
  EXPECT_EQ(set_.NextId(), 4u);
}

TEST_F(ContainerWriterTest, FullExactlyAtTheTarget) {
  // File size = 32 header + payload + 4 + 50 per entry + 32 trailer (format version 2). One
  // 1000-byte blob gives 1118 bytes.
  const auto p = Payload(3, 1000);
  const Fingerprint fp = FingerprintOf(BlobType::kData, p);
  {
    ContainerWriter w = MakeWriter(1119);
    w.Add(fp, BlobType::kData, p);
    EXPECT_EQ(w.FileBytes(), 1118u);
    EXPECT_FALSE(w.Full());  // just below the target
  }
  {
    ContainerWriter w = MakeWriter(1118);
    w.Add(fp, BlobType::kData, p);
    EXPECT_TRUE(w.Full());  // exactly at the target
    auto sealed = w.Seal();
    ASSERT_TRUE(sealed.ok());
    EXPECT_EQ(sealed->file_bytes, 1118u);
  }
}

TEST_F(ContainerWriterTest, SealNeverReplacesAnExistingContainer) {
  // Plant a file where the next container would go; sealing must fail rather than replace it.
  {
    auto f = fs_.CreateNew("/r/containers/" + ContainerFileName(1));
    ASSERT_TRUE(f.ok());
    ASSERT_TRUE((*f)->Append(AsBytes("precious")).ok());
    ASSERT_TRUE((*f)->Close().ok());
  }
  ContainerWriter w = MakeWriter(1 << 20);
  const auto p = Payload(4, 10);
  w.Add(FingerprintOf(BlobType::kData, p), BlobType::kData, p);
  auto sealed = w.Seal();
  EXPECT_EQ(sealed.status().code(), StatusCode::kCorruption);
  auto contents = ReadWholeFile(fs_, "/r/containers/" + ContainerFileName(1));
  ASSERT_TRUE(contents.ok());
  EXPECT_EQ(AsChars(*contents), "precious");
}

TEST_F(ContainerWriterTest, LargeMetadataBlob) {
  // A 64 MiB tree blob fits in one container (SPEC.md section 6).
  ContainerWriter w = MakeWriter(4 << 20);
  const std::vector<std::byte> big(kMaxMetadataBlobBytes, std::byte{7});
  w.Add(FingerprintOf(BlobType::kTree, big), BlobType::kTree, big);
  EXPECT_TRUE(w.Full());
  auto sealed = w.Seal();
  ASSERT_TRUE(sealed.ok()) << sealed.status().ToString();
  EXPECT_EQ(sealed->entries[0].loc.length, kMaxMetadataBlobBytes);
}

TEST_F(ContainerWriterTest, DuplicateFingerprintInOneContainerAborts) {
  ContainerWriter w = MakeWriter(1 << 20);
  const auto p = Payload(5, 10);
  const Fingerprint fp = FingerprintOf(BlobType::kData, p);
  w.Add(fp, BlobType::kData, p);
  EXPECT_DEATH(w.Add(fp, BlobType::kData, p), "check failed");
}

TEST_F(ContainerWriterTest, SealingAnEmptyContainerAborts) {
  ContainerWriter w = MakeWriter(1 << 20);
  EXPECT_DEATH((void)w.Seal(), "check failed");
}

// D107: with zstd, a compressible blob is stored as a frame smaller than its content and reads
// back as the content; random bytes, which zstd cannot shrink, are stored as they are.
TEST_F(ContainerWriterTest, CompressesOnlyWhenThatIsSmaller) {
  ContainerWriter w(fs_, "/r/containers", "/r/tmp", TestRepoId(), set_, 1 << 20,
                    CompressionParams{Codec::kZstd, 3});
  const std::vector<std::byte> text(20000, std::byte{'a'});
  const std::vector<std::byte> noise = Payload(9, 20000);
  w.Add(FingerprintOf(BlobType::kData, text), BlobType::kData, text);
  w.Add(FingerprintOf(BlobType::kData, noise), BlobType::kData, noise);
  auto sealed = w.Seal();
  ASSERT_TRUE(sealed.ok()) << sealed.status().ToString();
  const SealedEntry& small = sealed->entries[0];
  const SealedEntry& raw = sealed->entries[1];
  EXPECT_EQ(small.loc.codec, Codec::kZstd);
  EXPECT_LT(small.loc.length, 1000u);
  EXPECT_EQ(small.raw_length, 20000u);
  EXPECT_EQ(raw.loc.codec, Codec::kNone);
  EXPECT_EQ(raw.loc.length, 20000u);
  EXPECT_EQ(sealed->payload_bytes, uint64_t{small.loc.length} + raw.loc.length);
  EXPECT_EQ(*set_.Read(small.loc), text);
  EXPECT_EQ(*set_.Read(raw.loc), noise);
  auto meta = ReadContainerMetadata(fs_, "/r/containers/" + ContainerFileName(sealed->id),
                                    sealed->id, TestRepoId(), 1 << 20);
  ASSERT_TRUE(meta.ok()) << meta.status().ToString();
  EXPECT_EQ(meta->entries[0].loc, small.loc);
  EXPECT_EQ(meta->entries[0].raw_length, 20000u);
}

}  // namespace
}  // namespace dds
