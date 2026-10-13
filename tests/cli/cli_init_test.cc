// `dds init` (SPEC.md section 3) and its exit codes.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "support/run_dds.h"
#include "support/temp_dir.h"

namespace dds {
namespace {

using testing::RunDds;

TEST(CliInitTest, CreatesARepositoryAndReportsItsParameters) {
  testing::TempDir dir;
  const std::string repo = dir.Sub("repo");
  auto run = RunDds({"init", repo, "--json"});
  ASSERT_EQ(run.exit_code, 0) << run.err;
  EXPECT_TRUE(std::filesystem::exists(repo + "/config"));
  EXPECT_TRUE(std::filesystem::is_directory(repo + "/containers"));
  EXPECT_NE(run.out.find("\"chunker\":\"fastcdc-2016\""), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"avg_size\":8192"), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"container_target\":4194304"), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"repo_id\":\""), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"compression\":\"none\""), std::string::npos) << run.out;
}

// D107: --compression zstd (level 3) or zstd:<level>, recorded in the config; a backup and restore
// through the compressed repository give the files back, and stats shows the compression apart
// from dedup.
TEST(CliInitTest, CompressedRepositoryRoundTrips) {
  testing::TempDir dir;
  const std::string repo = dir.Sub("repo");
  auto run = RunDds({"init", repo, "--compression", "zstd", "--json"});
  ASSERT_EQ(run.exit_code, 0) << run.err;
  EXPECT_NE(run.out.find("\"compression\":\"zstd:3\""), std::string::npos) << run.out;
  std::ifstream config(repo + "/config");
  std::string text((std::istreambuf_iterator<char>(config)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("compression=zstd:3\n"), std::string::npos) << text;
  std::filesystem::create_directory(dir.Sub("src"));
  std::ofstream(dir.Sub("src/text")) << std::string(100000, 'q') << "tail";
  auto backup = RunDds({"backup", repo, dir.Sub("src")});
  ASSERT_EQ(backup.exit_code, 0) << backup.err;
  std::string id = backup.out.substr(0, 32);
  ASSERT_EQ(RunDds({"restore", repo, id, dir.Sub("out")}).exit_code, 0);
  std::ifstream restored(dir.Sub("out/text"));
  std::string bytes((std::istreambuf_iterator<char>(restored)), std::istreambuf_iterator<char>());
  EXPECT_EQ(bytes, std::string(100000, 'q') + "tail");
  EXPECT_EQ(RunDds({"verify", repo, "--deep"}).exit_code, 0);
  auto stats = RunDds({"stats", repo, "--json"});
  ASSERT_EQ(stats.exit_code, 0) << stats.err;
  EXPECT_NE(stats.out.find("\"unique_data_bytes\":100004,"), std::string::npos) << stats.out;
  EXPECT_EQ(stats.out.find("\"compression_ratio\":1,"), std::string::npos) << stats.out;
}

TEST(CliInitTest, CustomParameters) {
  testing::TempDir dir;
  const std::string repo = dir.Sub("repo");
  auto run = RunDds(
      {"init", repo, "--chunker", "fixed", "--avg-size", "4K", "--container-size", "1M", "--json"});
  ASSERT_EQ(run.exit_code, 0) << run.err;
  EXPECT_NE(run.out.find("\"chunker\":\"fixed\""), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"min_size\":4096"), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"container_target\":1048576"), std::string::npos) << run.out;
}

TEST(CliInitTest, ExistingRepositoryOrNonEmptyDirectoryIsAlreadyExists) {
  testing::TempDir dir;
  const std::string repo = dir.Sub("repo");
  ASSERT_EQ(RunDds({"init", repo}).exit_code, 0);
  EXPECT_EQ(RunDds({"init", repo}).exit_code, 9);
  std::filesystem::create_directory(dir.Sub("full"));
  std::ofstream(dir.Sub("full/file")) << "x";
  EXPECT_EQ(RunDds({"init", dir.Sub("full")}).exit_code, 9);
}

TEST(CliInitTest, BadParametersAreUsageErrors) {
  testing::TempDir dir;
  EXPECT_EQ(RunDds({"init", dir.Sub("a"), "--container-size", "1000"}).exit_code, 1);
  EXPECT_EQ(RunDds({"init", dir.Sub("b"), "--avg-size", "3000"}).exit_code, 1);
  EXPECT_EQ(RunDds({"init", dir.Sub("c"), "--chunker", "fixed", "--max-size", "64K"}).exit_code, 1);
  EXPECT_EQ(RunDds({"init"}).exit_code, 1);
  EXPECT_EQ(RunDds({"init", dir.Sub("d"), "--compression", "zstd:20"}).exit_code, 1);
  EXPECT_EQ(RunDds({"init", dir.Sub("e"), "--compression", "lz4"}).exit_code, 1);
  EXPECT_FALSE(std::filesystem::exists(dir.Sub("a")));
  EXPECT_FALSE(std::filesystem::exists(dir.Sub("d")));
}

TEST(CliInitTest, MissingParentDirectoryIsNotFound) {
  testing::TempDir dir;
  EXPECT_EQ(RunDds({"init", dir.Sub("no/such/parent")}).exit_code, 2);
}

}  // namespace
}  // namespace dds
