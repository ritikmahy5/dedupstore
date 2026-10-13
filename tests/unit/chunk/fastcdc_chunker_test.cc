#include "dds/chunk/fastcdc_chunker.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dds/chunk/chunker.h"
#include "dds/chunk/gear_table.h"
#include "support/random_data.h"
#include "support/test_seed.h"

namespace dds {
namespace {

// Algorithm 1 of the FastCDC paper (Xia et al., USENIX ATC 2016), transcribed line by line, with
// the paper's constants replaced by parameters. `n` is the length of `src`. Kept deliberately
// close to the pseudocode, so the production loop is checked against the paper and not against a
// second copy of itself.
size_t PaperAlgorithm1(const uint8_t* src, size_t n, size_t min_size, size_t normal_size,
                       size_t max_size, uint64_t mask_s, uint64_t mask_l,
                       const std::array<uint64_t, 256>& gear) {
  uint64_t fp = 0;
  size_t i = min_size;
  if (n <= min_size) return n;
  if (n >= max_size) {
    n = max_size;
  } else if (n <= normal_size) {
    normal_size = n;
  }
  for (; i < normal_size; i++) {
    fp = (fp << 1) + gear[src[i]];
    if (!(fp & mask_s)) return i;
  }
  for (; i < n; i++) {
    fp = (fp << 1) + gear[src[i]];
    if (!(fp & mask_l)) return i;
  }
  return i;
}

ChunkerParams FastCdc(uint32_t min, uint32_t avg, uint32_t max, int level, uint64_t seed) {
  ChunkerParams p;
  p.kind = ChunkerKind::kFastCdc;
  p.min_size = min;
  p.avg_size = avg;
  p.max_size = max;
  p.normalization = level;
  p.gear_seed = seed;
  return p;
}

TEST(FastCdcChunkerTest, MaskTableHasOneBitPerLevel) {
  // Index k of the table is the mask for 2^k: exactly k one bits (D62). This also checks the
  // transcription of the table from fastcdc-rs.
  for (int bits = 5; bits <= 25; ++bits) {
    EXPECT_EQ(std::popcount(FastCdcMask(bits)), bits) << "bits=" << bits;
    // All masks look at the top bits of a 48-bit window, as the paper's padded masks do.
    EXPECT_LT(FastCdcMask(bits), uint64_t{1} << 48) << "bits=" << bits;
  }
}

TEST(FastCdcChunkerTest, MasksAgreeWithThePaperWhereThePaperGivesThem) {
  // Algorithm 1 prints MaskA (13 one bits) and MaskL (11) for 8 KiB; the table has the same
  // values. The paper's MaskS (15 bits) is 0x0003590703530000; the table's 15-bit mask has the
  // same bit count but different positions. D62 records the choice.
  EXPECT_EQ(FastCdcMask(13), 0x0000d90303530000ULL);
  EXPECT_EQ(FastCdcMask(11), 0x0000d90003530000ULL);
  EXPECT_EQ(FastCdcMask(15), 0x0000d90f03530000ULL);
  EXPECT_EQ(std::popcount(uint64_t{0x0003590703530000ULL}), 15);
}

TEST(FastCdcChunkerTest, MatchesPaperAlgorithmOnRandomData) {
  const std::vector<ChunkerParams> param_sets = {
      FastCdc(2048, 8192, 65536, 0, kDefaultGearSeed),
      FastCdc(2048, 8192, 65536, 1, kDefaultGearSeed),
      FastCdc(2048, 8192, 65536, 2, kDefaultGearSeed),
      FastCdc(2048, 8192, 65536, 3, kDefaultGearSeed),
      FastCdc(64, 256, 1024, 2, kDefaultGearSeed),
      FastCdc(64, 256, 1024, 3, 12345),
      FastCdc(1024, 4096, 16384, 2, 99),
      FastCdc(16384, 65536, 524288, 1, kDefaultGearSeed),
  };
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    const std::vector<std::byte> data = testing::RandomBytes(seed, 2 << 20);
    for (const ChunkerParams& p : param_sets) {
      SCOPED_TRACE("min=" + std::to_string(p.min_size) + " avg=" + std::to_string(p.avg_size) +
                   " max=" + std::to_string(p.max_size) +
                   " level=" + std::to_string(p.normalization));
      auto chunker = MakeChunker(p);
      ASSERT_TRUE(chunker.ok()) << chunker.status().ToString();
      const auto gear = GenerateGearTable(p.gear_seed);
      const int bits = std::countr_zero(p.avg_size);
      const uint64_t mask_s = FastCdcMask(bits + p.normalization);
      const uint64_t mask_l = FastCdcMask(bits - p.normalization);
      size_t pos = 0;
      while (pos < data.size()) {
        const auto rest = std::span(data).subspan(pos);
        const size_t got = (*chunker)->NextChunkLength(rest, /*eof=*/true);
        const size_t want =
            PaperAlgorithm1(reinterpret_cast<const uint8_t*>(rest.data()), rest.size(), p.min_size,
                            p.avg_size, p.max_size, mask_s, mask_l, gear);
        ASSERT_EQ(got, want) << "at offset " << pos;
        pos += got;
      }
    }
  }
}

TEST(FastCdcChunkerTest, NeedsMoreInputUntilMaxBytesOrEof) {
  auto chunker = MakeChunker(FastCdc(2048, 8192, 65536, 2, kDefaultGearSeed));
  ASSERT_TRUE(chunker.ok());
  const std::vector<std::byte> data = testing::RandomBytes(1, 65536);
  // Fewer than max bytes and more may come: never decide, even if a cut point is visible, because
  // the decision could depend on where the stream happened to be split.
  EXPECT_EQ((*chunker)->NextChunkLength(std::span(data).first(65535), false), 0u);
  EXPECT_EQ((*chunker)->NextChunkLength(std::span(data).first(10), false), 0u);
  EXPECT_GT((*chunker)->NextChunkLength(data, false), 0u);
  EXPECT_GT((*chunker)->NextChunkLength(std::span(data).first(65535), true), 0u);
}

TEST(FastCdcChunkerTest, ShortTailAtEofIsOneChunk) {
  auto chunker = MakeChunker(FastCdc(2048, 8192, 65536, 2, kDefaultGearSeed));
  ASSERT_TRUE(chunker.ok());
  const std::vector<std::byte> data = testing::RandomBytes(2, 2048);
  EXPECT_EQ((*chunker)->NextChunkLength(std::span(data).first(1), true), 1u);
  EXPECT_EQ((*chunker)->NextChunkLength(data, true), 2048u);  // n <= min: the whole rest
  EXPECT_EQ((*chunker)->NextChunkLength({}, true), 0u);       // nothing left
}

TEST(FastCdcChunkerTest, NeverExceedsMax) {
  // Low-entropy input (all zeros) may never produce a cut point; the loop bound must then cut
  // at max_size.
  auto chunker = MakeChunker(FastCdc(64, 256, 1024, 2, kDefaultGearSeed));
  ASSERT_TRUE(chunker.ok());
  const std::vector<std::byte> zeros(10000, std::byte{0});
  size_t pos = 0;
  while (pos < zeros.size()) {
    const size_t len = (*chunker)->NextChunkLength(std::span(zeros).subspan(pos), true);
    ASSERT_GE(len, 1u);
    ASSERT_LE(len, 1024u);
    pos += len;
  }
}

TEST(FastCdcChunkerTest, ValidatesParameters) {
  EXPECT_TRUE(MakeChunker(FastCdc(64, 256, 1024, 0, 1)).ok());
  EXPECT_TRUE(MakeChunker(FastCdc(1048575, 4194304, 16777216, 3, 1)).ok());
  const StatusCode bad = StatusCode::kInvalidArgument;
  EXPECT_EQ(MakeChunker(FastCdc(63, 256, 1024, 2, 1)).status().code(), bad);          // min < 64
  EXPECT_EQ(MakeChunker(FastCdc(256, 256, 1024, 2, 1)).status().code(), bad);         // min >= avg
  EXPECT_EQ(MakeChunker(FastCdc(64, 300, 1024, 2, 1)).status().code(), bad);          // avg not 2^k
  EXPECT_EQ(MakeChunker(FastCdc(64, 128, 1024, 2, 1)).status().code(), bad);          // avg < 256
  EXPECT_EQ(MakeChunker(FastCdc(64, 8388608, 16777216, 2, 1)).status().code(), bad);  // > 4 MiB
  EXPECT_EQ(MakeChunker(FastCdc(64, 256, 256, 2, 1)).status().code(), bad);           // max <= avg
  EXPECT_EQ(MakeChunker(FastCdc(64, 256, 16777217, 2, 1)).status().code(), bad);  // max > 16 MiB
  EXPECT_EQ(MakeChunker(FastCdc(64, 256, 1024, 4, 1)).status().code(), bad);      // level > 3
  EXPECT_EQ(MakeChunker(FastCdc(64, 256, 1024, -1, 1)).status().code(), bad);
}

TEST(FastCdcChunkerTest, ReportsItsParameters) {
  auto chunker = MakeChunker(FastCdc(2048, 8192, 65536, 2, kDefaultGearSeed));
  ASSERT_TRUE(chunker.ok());
  EXPECT_EQ((*chunker)->MinSize(), 2048u);
  EXPECT_EQ((*chunker)->MaxSize(), 65536u);
  EXPECT_STREQ(ChunkerKindName(ChunkerKind::kFastCdc), "fastcdc-2016");
  EXPECT_STREQ(ChunkerKindName(ChunkerKind::kFixed), "fixed");
}

}  // namespace
}  // namespace dds
