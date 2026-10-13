#include "dds/chunk/chunk_reader.h"

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
  p.min_size = p.avg_size = p.max_size = size;
  p.normalization = 0;
  return p;
}

// Returns at most 1000 bytes per Read and fails with IoError on the n-th call (counting from 1).
// The small reads make the reader call Read many times, so the failing call is reached.
class FailingSource final : public ByteSource {
 public:
  FailingSource(std::vector<std::byte> data, int fail_on)
      : data_(std::move(data)), fail_on_(fail_on) {}
  Result<size_t> Read(std::span<std::byte> out) override {
    if (++calls_ == fail_on_) return Status::IoError("disk on fire");
    const size_t n = std::min({out.size(), data_.size() - pos_, size_t{1000}});
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), n, out.begin());
    pos_ += n;
    return n;
  }

 private:
  std::vector<std::byte> data_;
  size_t pos_ = 0;
  int calls_ = 0;
  int fail_on_;
};

TEST(ChunkReaderTest, EmptyStreamYieldsNoChunks) {
  auto chunker = MakeChunker(Fixed(256));
  ASSERT_TRUE(chunker.ok());
  SpanByteSource source({});
  ChunkReader reader(**chunker, source);
  auto next = reader.Next();
  ASSERT_TRUE(next.ok());
  EXPECT_FALSE(next->has_value());
  EXPECT_EQ(reader.bytes_read(), 0u);
}

TEST(ChunkReaderTest, YieldsTheStreamInOrder) {
  auto chunker = MakeChunker(Fixed(1024));
  ASSERT_TRUE(chunker.ok());
  const std::vector<std::byte> data = testing::RandomBytes(4, 10 * 1024 + 7);
  SpanByteSource source(data);
  ChunkReader reader(**chunker, source);
  std::vector<std::byte> joined;
  size_t chunks = 0;
  while (true) {
    auto next = reader.Next();
    ASSERT_TRUE(next.ok()) << next.status().ToString();
    if (!next->has_value()) break;
    joined.insert(joined.end(), (*next)->begin(), (*next)->end());
    ++chunks;
  }
  EXPECT_EQ(joined, data);
  EXPECT_EQ(chunks, 11u);
  EXPECT_EQ(reader.bytes_read(), data.size());
}

TEST(ChunkReaderTest, ReadErrorIsReturned) {
  auto chunker = MakeChunker(Fixed(1024));
  ASSERT_TRUE(chunker.ok());
  FailingSource source(testing::RandomBytes(5, 100000), /*fail_on=*/3);
  ChunkReader reader(**chunker, source);
  Status error;
  for (int i = 0; i < 1000 && error.ok(); ++i) {
    auto next = reader.Next();
    if (!next.ok()) error = next.status();
    if (next.ok() && !next->has_value()) break;
  }
  EXPECT_EQ(error.code(), StatusCode::kIoError);
}

std::vector<std::vector<std::byte>> AllChunks(ChunkReader& reader) {
  std::vector<std::vector<std::byte>> chunks;
  while (true) {
    auto next = reader.Next();
    EXPECT_TRUE(next.ok()) << next.status().ToString();
    if (!next.ok() || !next->has_value()) break;
    chunks.emplace_back((*next)->begin(), (*next)->end());
  }
  return chunks;
}

// A backup worker reads every file through one borrowed buffer. Stale bytes left by a longer stream
// must not change the next stream's chunks, and the buffer must be allocated once, not per stream
// (a per-stream allocation of 2 x the maximum chunk, zero-filled, dominated E2's 1 MiB runs).
TEST(ChunkReaderTest, ABorrowedBufferYieldsTheSameChunksAndIsAllocatedOnce) {
  auto chunker = MakeChunker(ChunkerParams{});
  ASSERT_TRUE(chunker.ok());
  std::vector<std::byte> shared;
  const std::byte* first_allocation = nullptr;
  uint64_t seed = 10;
  for (size_t size : {size_t{300000}, size_t{5000}, size_t{0}, size_t{150001}}) {
    const std::vector<std::byte> data = testing::RandomBytes(seed++, size);
    SpanByteSource borrowed_source(data, /*max_read=*/4096);
    ChunkReader borrowed(**chunker, borrowed_source, shared);
    SpanByteSource owned_source(data);
    ChunkReader owned(**chunker, owned_source);
    EXPECT_EQ(AllChunks(borrowed), AllChunks(owned)) << "stream of " << size << " bytes";
    EXPECT_GE(shared.size(), 2 * size_t{(*chunker)->MaxSize()});
    if (first_allocation == nullptr) first_allocation = shared.data();
    EXPECT_EQ(shared.data(), first_allocation) << "the buffer was reallocated";
  }
}

TEST(ChunkReaderTest, SpanSourceHonorsMaxReadSize) {
  const std::vector<std::byte> data = testing::RandomBytes(6, 100);
  SpanByteSource source(data, /*max_read=*/7);
  std::vector<std::byte> buf(64);
  auto n = source.Read(buf);
  ASSERT_TRUE(n.ok());
  EXPECT_EQ(*n, 7u);
}

}  // namespace
}  // namespace dds
