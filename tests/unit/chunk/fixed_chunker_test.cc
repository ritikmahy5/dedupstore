#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "dds/chunk/chunker.h"
#include "support/random_data.h"

namespace dds {
namespace {

ChunkerParams Fixed(uint32_t size) {
  ChunkerParams p;
  p.kind = ChunkerKind::kFixed;
  p.min_size = size;
  p.avg_size = size;
  p.max_size = size;
  p.normalization = 0;
  return p;
}

TEST(FixedChunkerTest, EveryChunkIsTheSizeExceptTheLast) {
  auto chunker = MakeChunker(Fixed(4096));
  ASSERT_TRUE(chunker.ok()) << chunker.status().ToString();
  const std::vector<std::byte> data = testing::RandomBytes(3, 4096 * 3 + 100);
  std::vector<size_t> lengths;
  size_t pos = 0;
  while (pos < data.size()) {
    const size_t len = (*chunker)->NextChunkLength(std::span(data).subspan(pos), true);
    ASSERT_GT(len, 0u);
    lengths.push_back(len);
    pos += len;
  }
  EXPECT_EQ(lengths, (std::vector<size_t>{4096, 4096, 4096, 100}));
}

TEST(FixedChunkerTest, NeedsMoreInputBelowTheSize) {
  auto chunker = MakeChunker(Fixed(256));
  ASSERT_TRUE(chunker.ok());
  const std::vector<std::byte> data(300, std::byte{7});
  EXPECT_EQ((*chunker)->NextChunkLength(std::span(data).first(255), false), 0u);
  EXPECT_EQ((*chunker)->NextChunkLength(std::span(data).first(256), false), 256u);
  EXPECT_EQ((*chunker)->NextChunkLength(std::span(data).first(255), true), 255u);
  EXPECT_EQ((*chunker)->NextChunkLength({}, true), 0u);
}

TEST(FixedChunkerTest, ValidatesParameters) {
  EXPECT_TRUE(MakeChunker(Fixed(8192)).ok());
  EXPECT_EQ(MakeChunker(Fixed(100)).status().code(), StatusCode::kInvalidArgument);  // not 2^k
  ChunkerParams mixed = Fixed(8192);
  mixed.min_size = 2048;  // fixed requires min = avg = max (ON_DISK_FORMAT.md section 3)
  EXPECT_EQ(MakeChunker(mixed).status().code(), StatusCode::kInvalidArgument);
  ChunkerParams level = Fixed(8192);
  level.normalization = 1;
  EXPECT_EQ(MakeChunker(level).status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace dds
