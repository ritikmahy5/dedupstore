// Chunker properties (TESTING.md 3.4).

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dds/chunk/chunk_reader.h"
#include "dds/chunk/chunker.h"
#include "dds/chunk/gear_table.h"
#include "dds/hash/fingerprint.h"
#include "support/byte_sources.h"
#include "support/random_data.h"
#include "support/test_seed.h"

namespace dds {
namespace {

ChunkerParams FastCdc(uint32_t min, uint32_t avg, uint32_t max, int level) {
  ChunkerParams p;
  p.kind = ChunkerKind::kFastCdc;
  p.min_size = min;
  p.avg_size = avg;
  p.max_size = max;
  p.normalization = level;
  return p;
}

ChunkerParams Fixed(uint32_t size) {
  ChunkerParams p;
  p.kind = ChunkerKind::kFixed;
  p.min_size = p.avg_size = p.max_size = size;
  p.normalization = 0;
  return p;
}

std::vector<ChunkerParams> AllParams() {
  return {FastCdc(2048, 8192, 65536, 2),
          FastCdc(2048, 8192, 65536, 0),
          FastCdc(2048, 8192, 65536, 3),
          FastCdc(64, 256, 1024, 1),
          FastCdc(4096, 16384, 131072, 2),
          Fixed(8192),
          Fixed(256)};
}

std::string Describe(const ChunkerParams& p) {
  return std::string(ChunkerKindName(p.kind)) + " " + std::to_string(p.min_size) + "/" +
         std::to_string(p.avg_size) + "/" + std::to_string(p.max_size) + " level " +
         std::to_string(p.normalization);
}

// Chunk lengths of `data` read through a ByteSource that returns at most `max_read` bytes per
// call (or, with max_read == 0, random amounts from `split_rng`).
std::vector<size_t> ChunkLengths(const Chunker& chunker, std::span<const std::byte> data,
                                 size_t max_read, std::mt19937_64* split_rng = nullptr) {
  std::unique_ptr<ByteSource> source;
  if (split_rng != nullptr) {
    source = std::make_unique<testing::RandomSplitByteSource>(data, *split_rng);
  } else {
    source = std::make_unique<SpanByteSource>(data, max_read);
  }
  ChunkReader reader(chunker, *source);
  std::vector<size_t> lengths;
  while (true) {
    auto next = reader.Next();
    EXPECT_TRUE(next.ok()) << next.status().ToString();
    if (!next.ok() || !next->has_value()) break;
    lengths.push_back((*next)->size());
  }
  return lengths;
}

TEST(ChunkerPropertyTest, LengthsWithinBoundsAndConcatenationIsTheInput) {
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    const std::vector<std::byte> data = testing::RandomBytes(seed, (3 << 20) + 12345);
    for (const ChunkerParams& p : AllParams()) {
      SCOPED_TRACE(Describe(p));
      auto chunker = MakeChunker(p);
      ASSERT_TRUE(chunker.ok());
      SpanByteSource source(data);
      ChunkReader reader(**chunker, source);
      std::vector<std::byte> joined;
      std::vector<size_t> lengths;
      while (true) {
        auto next = reader.Next();
        ASSERT_TRUE(next.ok());
        if (!next->has_value()) break;
        lengths.push_back((*next)->size());
        joined.insert(joined.end(), (*next)->begin(), (*next)->end());
      }
      ASSERT_FALSE(lengths.empty());
      for (size_t i = 0; i + 1 < lengths.size(); ++i) {
        ASSERT_GE(lengths[i], p.min_size) << "chunk " << i;
        ASSERT_LE(lengths[i], p.max_size) << "chunk " << i;
      }
      EXPECT_GE(lengths.back(), 1u);
      EXPECT_LE(lengths.back(), p.max_size);
      EXPECT_EQ(joined, data);
    }
  }
}

TEST(ChunkerPropertyTest, BoundariesDoNotDependOnHowTheInputIsFed) {
  for (uint64_t seed : testing::TestSeeds(2)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    const std::vector<std::byte> data = testing::RandomBytes(seed, 1 << 20);
    for (const ChunkerParams& p : AllParams()) {
      SCOPED_TRACE(Describe(p));
      auto chunker = MakeChunker(p);
      ASSERT_TRUE(chunker.ok());
      const std::vector<size_t> whole = ChunkLengths(**chunker, data, data.size());
      size_t covered = 0;
      for (size_t len : whole) covered += len;
      ASSERT_EQ(covered, data.size());  // otherwise equal lists could both be empty
      for (size_t split : {size_t{1}, size_t{7}, size_t{13}, size_t{4093}, size_t{65537}}) {
        EXPECT_EQ(ChunkLengths(**chunker, data, split), whole) << "split=" << split;
      }
      std::mt19937_64 split_rng(seed ^ 0x5bd1e995);
      EXPECT_EQ(ChunkLengths(**chunker, data, 0, &split_rng), whole) << "random splits";
    }
  }
}

// Bytes of `original` that lie in chunks whose content also appears as a chunk of `edited`.
size_t BytesInUnchangedChunks(const Chunker& chunker, std::span<const std::byte> original,
                              std::span<const std::byte> edited, size_t* first_changed_offset) {
  auto fingerprints = [&](std::span<const std::byte> data) {
    std::vector<std::pair<Fingerprint, size_t>> out;  // (fingerprint, length) in order
    size_t pos = 0;
    while (pos < data.size()) {
      const size_t len = chunker.NextChunkLength(data.subspan(pos), true);
      out.emplace_back(FingerprintOf(BlobType::kData, data.subspan(pos, len)), len);
      pos += len;
    }
    return out;
  };
  const auto before = fingerprints(original);
  std::set<Fingerprint> after;
  for (const auto& [fp, len] : fingerprints(edited)) after.insert(fp);
  size_t unchanged = 0;
  size_t offset = 0;
  *first_changed_offset = original.size();
  for (const auto& [fp, len] : before) {
    if (after.count(fp) > 0) {
      unchanged += len;
    } else if (*first_changed_offset == original.size()) {
      *first_changed_offset = offset;
    }
    offset += len;
  }
  return unchanged;
}

TEST(ChunkerPropertyTest, ShiftResistanceFastCdcVersusFixed) {
  // Insert one byte into the middle of 16 MiB of random data. Content-defined boundaries move with
  // the content, so nearly every chunk survives; fixed boundaries shift, so every chunk after the
  // insertion changes. Both halves are asserted: the contrast is the point (TESTING.md 3.4).
  const uint64_t seed = testing::TestSeed();
  SCOPED_TRACE(testing::SeedMessage(seed));
  const std::vector<std::byte> original = testing::RandomBytes(seed, 16 << 20);
  const size_t insert_at = original.size() / 2 + 12345;
  std::vector<std::byte> edited = original;
  edited.insert(edited.begin() + static_cast<std::ptrdiff_t>(insert_at), std::byte{0x5a});

  auto fastcdc = MakeChunker(FastCdc(2048, 8192, 65536, 2));
  ASSERT_TRUE(fastcdc.ok());
  size_t first_changed = 0;
  const size_t cdc_unchanged = BytesInUnchangedChunks(**fastcdc, original, edited, &first_changed);
  EXPECT_GE(static_cast<double>(cdc_unchanged), 0.95 * static_cast<double>(original.size()))
      << "unchanged bytes " << cdc_unchanged << " of " << original.size();

  auto fixed = MakeChunker(Fixed(8192));
  ASSERT_TRUE(fixed.ok());
  const size_t fixed_unchanged = BytesInUnchangedChunks(**fixed, original, edited, &first_changed);
  // Everything from the chunk containing the insertion point onward changed.
  EXPECT_LE(first_changed, insert_at);
  EXPECT_LE(fixed_unchanged, insert_at);
}

TEST(ChunkerPropertyTest, GoldenBoundaries) {
  // The boundaries of a fixed 1 MiB input under the default parameters (D8, D9), recorded once and
  // reviewed (tests/fixtures/fastcdc_golden_1mib.txt). A change to the gear table, the
  // masks, or the loop moves them, and with them every existing repository's dedup. Changing the
  // fixture requires a decision in DECISIONS.md.
  std::ifstream in(std::string(DDS_FIXTURES_DIR) + "/fastcdc_golden_1mib.txt");
  ASSERT_TRUE(in.good()) << "missing fixture";
  std::vector<size_t> expected;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    expected.push_back(std::stoul(line));
  }
  auto chunker = MakeChunker(FastCdc(2048, 8192, 65536, 2));
  ASSERT_TRUE(chunker.ok());
  const std::vector<std::byte> data = testing::RandomBytes(20261007, 1 << 20);
  EXPECT_EQ(ChunkLengths(**chunker, data, data.size()), expected);
}

}  // namespace
}  // namespace dds
