// `dds serve` and `dds replicate` through the built binary (TESTING.md 3.9).

#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/run_dds.h"
#include "support/temp_dir.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using testing::RunDds;

// The port `dds serve` printed: "serving <repo> on <host>:<port>...".
std::string PortFrom(const std::string& line) {
  const size_t on = line.find(" on ");
  const size_t colon = line.find(':', on);
  const size_t end = line.find_first_not_of("0123456789", colon + 1);
  return line.substr(colon + 1, end - colon - 1);
}

TEST(CliReplTest, ServeAndReplicateOverLoopback) {
  testing::TempDir dir;
  const std::string src = dir.Sub("src-repo");
  const std::string dst = dir.Sub("dst-repo");
  ASSERT_EQ(RunDds({"init", src}).exit_code, 0);
  ASSERT_EQ(RunDds({"init", dst}).exit_code, 0);
  std::filesystem::create_directories(dir.Sub("data/sub"));
  std::ofstream(dir.Sub("data/a")) << std::string(300000, 'a') << "end";
  std::ofstream(dir.Sub("data/sub/b")) << "bee";
  auto backup = RunDds({"backup", src, dir.Sub("data")});
  ASSERT_EQ(backup.exit_code, 0) << backup.err;
  const std::string id = backup.out.substr(0, 32);

  testing::DdsProcess serve({"serve", dst, "--listen", "127.0.0.1:0", "--gc-interval", "1s"});
  const std::string line = serve.WaitForLine("serving");
  ASSERT_FALSE(line.empty());
  const std::string to = "127.0.0.1:" + PortFrom(line);
  auto replicate = RunDds({"replicate", src, id.substr(0, 8), "--to", to, "--json"});
  ASSERT_EQ(replicate.exit_code, 0) << replicate.err;
  EXPECT_NE(replicate.out.find("\"blobs_sent\":"), std::string::npos) << replicate.out;
  // Again: nothing to send, and still one snapshot on the replica. The root is found whole.
  auto again = RunDds({"replicate", src, id, "--to", to, "--json"});
  ASSERT_EQ(again.exit_code, 0) << again.err;
  EXPECT_NE(again.out.find("\"blobs_sent\":0,"), std::string::npos) << again.out;
  EXPECT_NE(again.out.find("\"subtrees_pruned\":1,"), std::string::npos) << again.out;
  auto unpruned = RunDds({"replicate", src, id, "--to", to, "--no-prune", "--json"});
  ASSERT_EQ(unpruned.exit_code, 0) << unpruned.err;
  EXPECT_NE(unpruned.out.find("\"trees_checked\":0,"), std::string::npos) << unpruned.out;
  EXPECT_EQ(serve.Stop(SIGTERM), 0);

  auto restore = RunDds({"restore", dst, id, dir.Sub("restored")});
  ASSERT_EQ(restore.exit_code, 0) << restore.err;
  EXPECT_TRUE(testing::DiffMemTrees(testing::ReadDirectory(dir.Sub("data")),
                                    testing::ReadDirectory(dir.Sub("restored")))
                  .empty());
  EXPECT_EQ(RunDds({"verify", dst, "--deep"}).exit_code, 0);
}

TEST(CliReplTest, ExitCodes) {
  testing::TempDir dir;
  const std::string src = dir.Sub("src-repo");
  ASSERT_EQ(RunDds({"init", src}).exit_code, 0);
  std::filesystem::create_directory(dir.Sub("data"));
  std::ofstream(dir.Sub("data/a")) << "x";
  const std::string id = RunDds({"backup", src, dir.Sub("data")}).out.substr(0, 32);
  EXPECT_EQ(RunDds({"replicate", src, id}).exit_code, 1);                     // no --to
  EXPECT_EQ(RunDds({"replicate", src, id, "--to", "nowhere"}).exit_code, 1);  // no port
  EXPECT_EQ(RunDds({"serve", src}).exit_code, 1);                             // no --listen
  EXPECT_EQ(RunDds({"serve", src, "--listen", "127.0.0.1:0", "--gc-interval", "5"}).exit_code, 1);
  // Nothing listens on port 1 (privileged and unused): connect fails, an I/O error.
  EXPECT_EQ(RunDds({"replicate", src, id, "--to", "127.0.0.1:1"}).exit_code, 5);
  // An incompatible replica: different chunker.
  const std::string fixed = dir.Sub("fixed-repo");
  ASSERT_EQ(RunDds({"init", fixed, "--chunker", "fixed"}).exit_code, 0);
  testing::DdsProcess serve({"serve", fixed, "--listen", "127.0.0.1:0"});
  const std::string line = serve.WaitForLine("serving");
  ASSERT_FALSE(line.empty());
  auto refused = RunDds({"replicate", src, id, "--to", "127.0.0.1:" + PortFrom(line)});
  EXPECT_EQ(refused.exit_code, 7) << refused.err;
}

}  // namespace
}  // namespace dds
