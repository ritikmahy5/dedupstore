// `dds chunk-stats` (SPEC.md section 3): a development tool that chunks one file and touches no
// repository.

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "dds/chunk/chunk_reader.h"
#include "dds/chunk/chunker.h"
#include "support/random_data.h"
#include "support/run_dds.h"
#include "support/temp_dir.h"

namespace dds {
namespace {

using testing::RunDds;

std::string WriteRandomFile(const testing::TempDir& dir, size_t size, uint64_t seed) {
  const std::string path = dir.Sub("input.bin");
  const std::vector<std::byte> data = testing::RandomBytes(seed, size);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return path;
}

size_t CountChunksInProcess(size_t size, uint64_t seed) {
  ChunkerParams params;  // defaults: FastCDC 2K / 8K / 64K, level 2
  auto chunker = MakeChunker(params);
  const std::vector<std::byte> data = testing::RandomBytes(seed, size);
  SpanByteSource source(data);
  ChunkReader reader(**chunker, source);
  size_t n = 0;
  while (true) {
    auto next = reader.Next();
    if (!next.ok() || !next->has_value()) break;
    ++n;
  }
  return n;
}

TEST(CliChunkStatsTest, JsonReportsTheSameChunkCountAsTheLibrary) {
  testing::TempDir dir;
  const std::string file = WriteRandomFile(dir, 3 << 20, 77);
  auto run = RunDds({"chunk-stats", file, "--json"});
  ASSERT_EQ(run.exit_code, 0) << run.err;
  // Exactly one JSON object on stdout.
  ASSERT_FALSE(run.out.empty());
  EXPECT_EQ(run.out.front(), '{');
  EXPECT_EQ(run.out.substr(run.out.size() - 2), "}\n");
  const std::string want = "\"chunks\":" + std::to_string(CountChunksInProcess(3 << 20, 77)) + ",";
  EXPECT_NE(run.out.find(want), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"bytes\":3145728,"), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"histogram\":"), std::string::npos) << run.out;
}

TEST(CliChunkStatsTest, HumanOutputGoesToStderr) {
  testing::TempDir dir;
  const std::string file = WriteRandomFile(dir, 1 << 20, 78);
  auto run = RunDds({"chunk-stats", file, "--chunker", "fixed", "--avg-size", "4K"});
  ASSERT_EQ(run.exit_code, 0) << run.err;
  EXPECT_TRUE(run.out.empty()) << run.out;
  EXPECT_NE(run.err.find("chunks: 256"), std::string::npos) << run.err;  // 1 MiB / 4 KiB
}

TEST(CliChunkStatsTest, MissingFileIsNotFound) {
  testing::TempDir dir;
  auto run = RunDds({"chunk-stats", dir.Sub("nope")});
  EXPECT_EQ(run.exit_code, 2) << run.err;
}

TEST(CliChunkStatsTest, BadSizesAreUsageErrors) {
  testing::TempDir dir;
  const std::string file = WriteRandomFile(dir, 1000, 79);
  EXPECT_EQ(RunDds({"chunk-stats", file, "--avg-size", "7000"}).exit_code, 1);  // not 2^k
  EXPECT_EQ(RunDds({"chunk-stats", file, "--avg-size", "8KB"}).exit_code, 1);   // bad suffix
  EXPECT_EQ(RunDds({"chunk-stats", file, "--chunker", "rabin"}).exit_code, 1);
  // SPEC.md section 3: with --chunker fixed, --min-size and --max-size are rejected.
  EXPECT_EQ(RunDds({"chunk-stats", file, "--chunker", "fixed", "--min-size", "1K"}).exit_code, 1);
}

}  // namespace
}  // namespace dds
