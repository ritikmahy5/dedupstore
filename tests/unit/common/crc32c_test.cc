#include "dds/common/crc32c.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dds/common/encoding.h"
#include "support/test_seed.h"

namespace dds {
namespace {

TEST(Crc32cTest, CheckValue) {
  // ON_DISK_FORMAT.md section 1: the standard CRC-32C check value.
  EXPECT_EQ(Crc32c(AsBytes(std::string_view("123456789"))), 0xE3069283u);
  EXPECT_EQ(Crc32cBytewise(AsBytes(std::string_view("123456789"))), 0xE3069283u);
  EXPECT_EQ(Crc32cExtendSoftware(0, AsBytes(std::string_view("123456789"))), 0xE3069283u);
}

TEST(Crc32cTest, EmptyIsZero) {
  EXPECT_EQ(Crc32c({}), 0u);
  EXPECT_EQ(Crc32cBytewise({}), 0u);
  EXPECT_EQ(Crc32cExtendSoftware(0, {}), 0u);
}

TEST(Crc32cTest, KnownVectors) {
  // RFC 3720 (iSCSI) appendix B.4 test vectors for CRC32C.
  std::vector<std::byte> zeros(32, std::byte{0});
  EXPECT_EQ(Crc32c(zeros), 0x8A9136AAu);
  std::vector<std::byte> ones(32, std::byte{0xFF});
  EXPECT_EQ(Crc32c(ones), 0x62A8AB43u);
  std::vector<std::byte> inc(32);
  for (size_t i = 0; i < inc.size(); ++i) inc[i] = static_cast<std::byte>(i);
  EXPECT_EQ(Crc32c(inc), 0x46DD794Eu);
  std::vector<std::byte> dec(32);
  for (size_t i = 0; i < dec.size(); ++i) dec[i] = static_cast<std::byte>(31 - i);
  EXPECT_EQ(Crc32c(dec), 0x113FDB5Cu);
}

TEST(Crc32cTest, ExtendEqualsWhole) {
  std::string_view text = "The quick brown fox jumps over the lazy dog";
  std::span<const std::byte> all = AsBytes(text);
  for (size_t split = 0; split <= all.size(); ++split) {
    uint32_t a = Crc32c(all.first(split));
    EXPECT_EQ(Crc32cExtend(a, all.subspan(split)), Crc32c(all)) << split;
  }
}

// Checks `crc` against Crc32cBytewise for every length up to 300 at several alignments, then on a
// few long random slices.
void ExpectAgreesWithBytewise(uint32_t (*crc)(std::span<const std::byte>)) {
  for (uint64_t seed : testing::TestSeeds(5)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    std::vector<std::byte> data(70000);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    for (size_t offset = 0; offset < 9; ++offset) {
      for (size_t len = 0; len <= 300; ++len) {
        auto slice = std::span<const std::byte>(data).subspan(offset, len);
        ASSERT_EQ(crc(slice), Crc32cBytewise(slice)) << "offset=" << offset << " len=" << len;
      }
    }
    for (int i = 0; i < 20; ++i) {
      size_t offset = rng() % 1000;
      size_t len = rng() % (data.size() - offset);
      auto slice = std::span<const std::byte>(data).subspan(offset, len);
      ASSERT_EQ(crc(slice), Crc32cBytewise(slice)) << "offset=" << offset << " len=" << len;
    }
  }
}

uint32_t Software(std::span<const std::byte> data) { return Crc32cExtendSoftware(0, data); }

TEST(Crc32cTest, SlicingAgreesWithBytewiseOnRandomInput) { ExpectAgreesWithBytewise(Software); }

// Crc32c uses the CPU's instructions where it has them (D103), and D26 requires them to match the
// table version on random input.
TEST(Crc32cTest, Crc32cAgreesWithBytewiseOnRandomInput) { ExpectAgreesWithBytewise(Crc32c); }

TEST(Crc32cTest, UsesTheHardwareWhereTheCpuHasIt) {
  RecordProperty("crc32c", std::string(Crc32cImplementation()));
#if defined(__x86_64__)
  __builtin_cpu_init();
  EXPECT_EQ(Crc32cImplementation(), __builtin_cpu_supports("sse4.2") ? "sse4.2" : "slicing-by-8");
#elif defined(__ARM_FEATURE_CRC32)
  EXPECT_EQ(Crc32cImplementation(), "armv8-crc");
#else
  EXPECT_EQ(Crc32cImplementation(), "slicing-by-8");
#endif
}

TEST(Crc32cTest, DetectsEverySingleBitFlip) {
  std::vector<std::byte> data(64);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>(i * 7 + 3);
  const uint32_t good = Crc32c(data);
  for (size_t i = 0; i < data.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      data[i] ^= static_cast<std::byte>(1 << bit);
      EXPECT_NE(Crc32c(data), good) << "byte " << i << " bit " << bit;
      data[i] ^= static_cast<std::byte>(1 << bit);
    }
  }
}

}  // namespace
}  // namespace dds

namespace dds {
namespace {

// The slow version of the AgreesWithBytewise tests: many more seeds and longer inputs.
TEST(Crc32cSlowTest, SlicingAgreesWithBytewiseManySeeds) {
  for (uint64_t seed : testing::TestSeeds(200)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    std::vector<std::byte> data(1 << 20);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    const size_t offset = rng() % 64;
    const size_t len = data.size() - offset - rng() % 64;
    auto slice = std::span<const std::byte>(data).subspan(offset, len);
    const uint32_t expected = Crc32cBytewise(slice);
    ASSERT_EQ(Crc32cExtendSoftware(0, slice), expected);
    ASSERT_EQ(Crc32c(slice), expected);
  }
}

}  // namespace
}  // namespace dds
