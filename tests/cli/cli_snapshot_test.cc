// The snapshot commands through the built binary (TESTING.md 3.9): init -> backup -> snapshots ->
// ls -> restore -> compare -> rm -> verify, and a test provoking each exit code these commands can
// return (SPEC.md section 3).

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "dds/common/process_identity.h"
#include "dds/repo/repo_lock.h"
#include "support/run_dds.h"
#include "support/temp_dir.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using testing::RunDds;

class CliSnapshotTest : public ::testing::Test {
 protected:
  void SetUp() override {
    repo_ = dir_.Sub("repo");
    src_ = dir_.Sub("src");
    ASSERT_EQ(RunDds({"init", repo_}).exit_code, 0);
    namespace fs = std::filesystem;
    fs::create_directories(src_ + "/sub/deeper");
    fs::create_directory(src_ + "/empty");
    std::ofstream(src_ + "/a.txt") << "alpha";
    std::ofstream(src_ + "/empty-file");
    std::ofstream(src_ + "/caf\xc3\xa9.txt") << "non-ascii name";
    std::ofstream(src_ + "/sub/deeper/data") << std::string(200000, 'z') << "end";
    fs::create_symlink("../a.txt", src_ + "/sub/link");
    ASSERT_EQ(::chmod((src_ + "/sub/deeper").c_str(), 0555), 0);
  }

  // Backs up src_ and returns the snapshot id printed on stdout.
  std::string Backup(const std::vector<std::string>& extra = {}) {
    std::vector<std::string> args = {"backup", repo_, src_};
    args.insert(args.end(), extra.begin(), extra.end());
    auto run = RunDds(args);
    EXPECT_EQ(run.exit_code, 0) << run.err;
    std::string id = run.out;
    while (!id.empty() && id.back() == '\n') id.pop_back();
    EXPECT_EQ(id.size(), 32u) << run.out;
    return id;
  }

  // A blob-region byte of the first container, flipped, so CRC and BLAKE3 checks fail.
  void DamageFirstContainer() {
    const std::string file = repo_ + "/containers/00000001.ctr";
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(40);  // past the 32-byte header, inside the first blob
    char c = 0;
    f.read(&c, 1);
    f.seekp(40);
    c = static_cast<char>(c ^ 0x10);
    f.write(&c, 1);
  }

  testing::TempDir dir_;
  std::string repo_;
  std::string src_;
};

TEST_F(CliSnapshotTest, FullCycle) {
  const std::string id = Backup({"--tag", "first"});

  auto list = RunDds({"snapshots", repo_, "--json"});
  ASSERT_EQ(list.exit_code, 0) << list.err;
  EXPECT_NE(list.out.find("\"id\":\"" + id + "\""), std::string::npos) << list.out;
  EXPECT_NE(list.out.find("\"tag\":\"first\""), std::string::npos) << list.out;
  EXPECT_NE(list.out.find("\"files\":4"), std::string::npos) << list.out;

  auto ls = RunDds({"ls", repo_, id.substr(0, 8), "sub", "--json"});
  ASSERT_EQ(ls.exit_code, 0) << ls.err;
  EXPECT_NE(ls.out.find("{\"name\":\"link\",\"kind\":\"symlink\""), std::string::npos) << ls.out;
  EXPECT_NE(ls.out.find("\"target\":\"../a.txt\""), std::string::npos) << ls.out;

  auto restore = RunDds({"restore", repo_, id.substr(0, 6), dir_.Sub("dest")});
  ASSERT_EQ(restore.exit_code, 0) << restore.err;
  const auto diffs =
      testing::DiffMemTrees(testing::ReadDirectory(src_), testing::ReadDirectory(dir_.Sub("dest")));
  EXPECT_TRUE(diffs.empty()) << ::testing::PrintToString(diffs);

  auto verify = RunDds({"verify", repo_, "--deep", "--json"});
  EXPECT_EQ(verify.exit_code, 0) << verify.err;
  EXPECT_NE(verify.out.find("\"ok\":true"), std::string::npos) << verify.out;

  auto stats = RunDds({"stats", repo_, "--json"});
  EXPECT_EQ(stats.exit_code, 0) << stats.err;
  EXPECT_NE(stats.out.find("\"snapshots\":1"), std::string::npos) << stats.out;

  EXPECT_EQ(RunDds({"rm", repo_, id}).exit_code, 0);
  list = RunDds({"snapshots", repo_, "--json"});
  EXPECT_EQ(list.out, "{\"snapshots\":[]}\n");
  verify = RunDds({"verify", repo_});
  EXPECT_EQ(verify.exit_code, 0) << verify.err;
}

TEST_F(CliSnapshotTest, SecondBackupOfAnUnchangedTreeStoresNothing) {
  Backup();
  auto run = RunDds({"backup", repo_, src_, "--json"});
  ASSERT_EQ(run.exit_code, 0) << run.err;
  EXPECT_NE(run.out.find("\"blobs_written\":0,"), std::string::npos) << run.out;
}

// D106: --parent auto (the default) picks the newest snapshot of the same path, whose unchanged
// files are not read; --parent none reads everything. The files are backdated a minute, past the
// 2-second margin a file's mtime needs before the parent's start.
TEST_F(CliSnapshotTest, SecondBackupSkipsUnchangedFilesUnlessTheParentIsNone) {
  namespace fs = std::filesystem;
  for (const auto& entry : fs::recursive_directory_iterator(src_)) {
    if (entry.is_regular_file() && !entry.is_symlink()) {
      fs::last_write_time(entry.path(), fs::file_time_type::clock::now() - std::chrono::minutes(1));
    }
  }
  const std::string first = Backup();
  auto run = RunDds({"backup", repo_, src_, "--json"});
  ASSERT_EQ(run.exit_code, 0) << run.err;
  EXPECT_NE(run.out.find("\"parent\":\"" + first + "\""), std::string::npos) << run.out;
  EXPECT_NE(run.out.find("\"files_reused\":4,"), std::string::npos) << run.out;
  auto none = RunDds({"backup", repo_, src_, "--json", "--parent", "none"});
  ASSERT_EQ(none.exit_code, 0) << none.err;
  EXPECT_NE(none.out.find("\"parent\":null"), std::string::npos) << none.out;
  EXPECT_NE(none.out.find("\"files_reused\":0,"), std::string::npos) << none.out;
  auto named = RunDds({"backup", repo_, src_, "--json", "--parent", first.substr(0, 8)});
  ASSERT_EQ(named.exit_code, 0) << named.err;
  EXPECT_NE(named.out.find("\"files_reused\":4,"), std::string::npos) << named.out;
}

TEST_F(CliSnapshotTest, RmResolvesEveryArgumentBeforeRemovingAny) {
  const std::string id = Backup();
  EXPECT_EQ(RunDds({"rm", repo_, id, "ffffff"}).exit_code, 2);
  auto list = RunDds({"snapshots", repo_, "--json"});
  EXPECT_NE(list.out.find(id), std::string::npos) << "nothing may be removed";
}

TEST_F(CliSnapshotTest, GcReclaimsWhatRmFreedAndDryRunChangesNothing) {
  const std::string first = Backup();
  std::ofstream(src_ + "/a.txt") << std::string(100000, 'q');  // the first snapshot's a.txt dies
  const std::string second = Backup();
  ASSERT_EQ(RunDds({"rm", repo_, first}).exit_code, 0);
  const auto listing = [&] {
    std::string out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(repo_)) {
      out += e.path().string() + " " +
             (e.is_regular_file() ? std::to_string(e.file_size()) : std::string("dir")) + "\n";
    }
    return out;
  };
  const std::string before = listing();
  auto dry = RunDds({"gc", repo_, "--dry-run", "--json"});
  ASSERT_EQ(dry.exit_code, 0) << dry.err;
  EXPECT_NE(dry.out.find("\"dry_run\":true"), std::string::npos) << dry.out;
  EXPECT_EQ(listing(), before);
  auto gc = RunDds({"gc", repo_, "--threshold", "1", "--json"});
  ASSERT_EQ(gc.exit_code, 0) << gc.err;
  EXPECT_EQ(gc.out.find("\"blobs_removed\":0,"), std::string::npos) << gc.out;
  EXPECT_EQ(RunDds({"verify", repo_, "--deep"}).exit_code, 0);
  auto restore = RunDds({"restore", repo_, second, dir_.Sub("dest")});
  ASSERT_EQ(restore.exit_code, 0) << restore.err;
  EXPECT_TRUE(
      testing::DiffMemTrees(testing::ReadDirectory(src_), testing::ReadDirectory(dir_.Sub("dest")))
          .empty());
  EXPECT_EQ(RunDds({"gc", repo_, "--threshold", "1.5"}).exit_code, 1);
  EXPECT_EQ(RunDds({"gc", repo_, "--threshold", "0"}).exit_code, 1);
}

// D109: --split-size sets the size from which a file is split across workers; 0 never splits.
TEST_F(CliSnapshotTest, SplitSizeChoosesWhichFilesAreSplit) {
  auto split = RunDds({"backup", repo_, src_, "--json", "--threads", "4", "--split-size", "100K"});
  ASSERT_EQ(split.exit_code, 0) << split.err;
  EXPECT_NE(split.out.find("\"files_split\":1,"), std::string::npos)
      << split.out;  // sub/deeper/data
  auto never = RunDds(
      {"backup", repo_, src_, "--json", "--threads", "4", "--split-size", "0", "--parent", "none"});
  ASSERT_EQ(never.exit_code, 0) << never.err;
  EXPECT_NE(never.out.find("\"files_split\":0,"), std::string::npos) << never.out;
  EXPECT_NE(never.out.find("\"blobs_written\":0,"), std::string::npos) << never.out;  // same blobs
}

TEST_F(CliSnapshotTest, Exit1UsageErrors) {
  EXPECT_EQ(RunDds({"backup", repo_}).exit_code, 1);
  EXPECT_EQ(RunDds({"restore", repo_, "abcdef"}).exit_code, 1);
  EXPECT_EQ(RunDds({"verify", repo_, "--shallow"}).exit_code, 1);
  EXPECT_EQ(RunDds({"backup", repo_, src_, "--threads", "0"}).exit_code, 1);
  EXPECT_EQ(RunDds({"backup", repo_, src_, "--split-size", "1G"}).exit_code, 1);
  EXPECT_EQ(RunDds({"rm", repo_}).exit_code, 1);
}

TEST_F(CliSnapshotTest, Exit2NotFound) {
  const std::string id = Backup();
  EXPECT_EQ(RunDds({"snapshots", dir_.Sub("no-such-repo")}).exit_code, 2);
  EXPECT_EQ(RunDds({"backup", repo_, dir_.Sub("no-such-dir")}).exit_code, 2);
  EXPECT_EQ(RunDds({"restore", repo_, "abcdef", dir_.Sub("d")}).exit_code, 2);
  EXPECT_EQ(RunDds({"ls", repo_, id, "sub/nope"}).exit_code, 2);
  EXPECT_EQ(RunDds({"ls", repo_, id, "a.txt"}).exit_code, 2);  // not a directory
}

TEST_F(CliSnapshotTest, Exit3Corruption) {
  const std::string id = Backup();
  DamageFirstContainer();
  auto verify = RunDds({"verify", repo_, "--deep"});
  EXPECT_EQ(verify.exit_code, 3) << verify.err;
  EXPECT_NE(verify.err.find("snapshot " + id), std::string::npos) << verify.err;
  EXPECT_EQ(RunDds({"restore", repo_, id, dir_.Sub("dest")}).exit_code, 3);
}

TEST_F(CliSnapshotTest, Exit4BusyWhileAnotherLiveProcessHoldsTheLock) {
  // This test process is alive, so a lock record naming it belongs to a live holder.
  const PosixProcessIdentity self;
  std::ofstream(repo_ + "/lock") << EncodeLockRecord(
      {self.Host(), self.Pid(), self.PidStart(), 1, "dds test"});
  auto run = RunDds({"snapshots", repo_});
  EXPECT_EQ(run.exit_code, 4) << run.err;
}

TEST_F(CliSnapshotTest, Exit5IoError) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  const std::string id = Backup();
  std::filesystem::create_directory(dir_.Sub("ro"));
  ASSERT_EQ(::chmod(dir_.Sub("ro").c_str(), 0555), 0);
  auto run = RunDds({"restore", repo_, id, dir_.Sub("ro/dest")});  // mkdir fails with EACCES
  EXPECT_EQ(run.exit_code, 5) << run.err;
}

TEST_F(CliSnapshotTest, Exit7IncompatibleRepositoryVersion) {
  std::ifstream in(repo_ + "/config");
  std::string config((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  config.replace(0, config.find('\n'), "dedupstore-config=3");
  std::ofstream(repo_ + "/config", std::ios::trunc) << config;
  auto run = RunDds({"snapshots", repo_});
  EXPECT_EQ(run.exit_code, 7) << run.err;
}

TEST_F(CliSnapshotTest, Exit8InvalidArgument) {
  auto run = RunDds({"backup", repo_, src_, "--tag", std::string(1025, 't')});
  EXPECT_EQ(run.exit_code, 8) << run.err;
}

TEST_F(CliSnapshotTest, Exit9AlreadyExists) {
  const std::string id = Backup();
  std::filesystem::create_directory(dir_.Sub("full"));
  std::ofstream(dir_.Sub("full/x")) << "x";
  auto run = RunDds({"restore", repo_, id, dir_.Sub("full")});
  EXPECT_EQ(run.exit_code, 9) << run.err;
}

}  // namespace
}  // namespace dds
