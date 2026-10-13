// CrashSimFs's model of a power loss (TESTING.md 3.6, D76): exactly what assumptions A1-A3
// promise survives, and anything else may or may not.

#include "dds/fs/crash_sim_fs.h"

#include <gtest/gtest.h>

#include <string>

#include "dds/common/encoding.h"

namespace dds {
namespace {

constexpr uint64_t kSeeds = 64;  // 0 and 1 are the extremes; the rest are random

void Write(Fs& fs, const std::string& path, std::string_view text, bool sync) {
  auto f = fs.CreateNew(path);
  ASSERT_TRUE(f.ok()) << f.status().ToString();
  ASSERT_TRUE((*f)->Append(AsBytes(text)).ok());
  if (sync) {
    ASSERT_TRUE((*f)->Sync().ok());
  }
  ASSERT_TRUE((*f)->Close().ok());
}

// The file's content after the crash, or "<absent>".
std::string Read(MemFs& fs, const std::string& path) {
  auto data = ReadWholeFile(fs, path);
  if (!data.ok()) return "<absent>";
  return std::string(AsChars(*data));
}

class CrashSimFsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (const char* dir : {"/tmp", "/c"}) ASSERT_TRUE(fs_.CreateDir(dir).ok());
    ASSERT_TRUE(fs_.SyncDir("/").ok());
  }
  CrashSimFs fs_;
};

TEST_F(CrashSimFsTest, SyncedBytesSurviveAndUnsyncedBytesMayNot) {
  auto f = fs_.CreateNew("/c/f");
  ASSERT_TRUE(f.ok());
  ASSERT_TRUE((*f)->Append(AsBytes("abc")).ok());
  ASSERT_TRUE((*f)->Sync().ok());
  ASSERT_TRUE((*f)->Append(AsBytes("def")).ok());
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  EXPECT_EQ(Read(*fs_.Materialize(0), "/c/f"), "abc");
  EXPECT_EQ(Read(*fs_.Materialize(1), "/c/f"), "abcdef");
  std::set<std::string> seen;
  for (uint64_t seed = 2; seed < kSeeds; ++seed) seen.insert(Read(*fs_.Materialize(seed), "/c/f"));
  EXPECT_EQ(seen, (std::set<std::string>{"abc", "abcd", "abcde", "abcdef"}));
}

TEST_F(CrashSimFsTest, ACreateIsDurableOnlyAfterItsDirectoryIsSynced) {
  Write(fs_, "/c/f", "x", /*sync=*/true);
  EXPECT_EQ(Read(*fs_.Materialize(0), "/c/f"), "<absent>");
  EXPECT_EQ(Read(*fs_.Materialize(1), "/c/f"), "x");
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  for (uint64_t seed = 0; seed < kSeeds; ++seed)
    EXPECT_EQ(Read(*fs_.Materialize(seed), "/c/f"), "x");
}

TEST_F(CrashSimFsTest, ARenameIsAtomicAndDurableOnlyThroughItsTargetDirectory) {
  Write(fs_, "/tmp/x", "data", /*sync=*/true);
  ASSERT_TRUE(fs_.SyncDir("/tmp").ok());
  ASSERT_TRUE(fs_.Rename("/tmp/x", "/c/x").ok());
  ASSERT_TRUE(fs_.SyncDir("/tmp").ok());  // the source directory: not enough
  bool saw_old = false, saw_new = false;
  for (uint64_t seed = 0; seed < kSeeds; ++seed) {
    auto disk = fs_.Materialize(seed);
    const bool old_name = Read(*disk, "/tmp/x") == "data";
    const bool new_name = Read(*disk, "/c/x") == "data";
    EXPECT_NE(old_name, new_name) << "seed " << seed << ": A3, never both and never neither";
    saw_old |= old_name;
    saw_new |= new_name;
  }
  EXPECT_TRUE(saw_old && saw_new);
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  for (uint64_t seed = 0; seed < kSeeds; ++seed) {
    auto disk = fs_.Materialize(seed);
    EXPECT_EQ(Read(*disk, "/c/x"), "data");
    EXPECT_EQ(Read(*disk, "/tmp/x"), "<absent>");
  }
}

TEST_F(CrashSimFsTest, ARenamedFileNeedsNoDurableOldName) {
  // The seal protocol (7.2): tmp/ is never synced, yet after the rename and SyncDir(containers/)
  // the container is durable, and a later SyncDir(tmp/) must not bring the old name back.
  Write(fs_, "/tmp/y", "sealed", /*sync=*/true);
  ASSERT_TRUE(fs_.Rename("/tmp/y", "/c/y").ok());
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  ASSERT_TRUE(fs_.SyncDir("/tmp").ok());
  for (uint64_t seed = 0; seed < kSeeds; ++seed) {
    auto disk = fs_.Materialize(seed);
    EXPECT_EQ(Read(*disk, "/c/y"), "sealed");
    EXPECT_EQ(Read(*disk, "/tmp/y"), "<absent>");
  }
}

TEST_F(CrashSimFsTest, ARemoveMayBeUndoneUntilItsDirectoryIsSynced) {
  Write(fs_, "/c/a", "a", /*sync=*/true);
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  ASSERT_TRUE(fs_.Remove("/c/a").ok());
  EXPECT_EQ(Read(*fs_.Materialize(0), "/c/a"), "a");
  EXPECT_EQ(Read(*fs_.Materialize(1), "/c/a"), "<absent>");
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  EXPECT_EQ(Read(*fs_.Materialize(0), "/c/a"), "<absent>");
}

TEST_F(CrashSimFsTest, ADirectoryAndItsContentsNeedTheParentSynced) {
  ASSERT_TRUE(fs_.CreateDir("/c/n").ok());
  Write(fs_, "/c/n/f", "f", /*sync=*/true);
  ASSERT_TRUE(fs_.SyncDir("/c/n").ok());
  auto none = fs_.Materialize(0);
  EXPECT_FALSE(*none->Exists("/c/n"));
  EXPECT_EQ(Read(*none, "/c/n/f"), "<absent>");  // durable in /c/n, but /c/n itself is not
  EXPECT_EQ(Read(*fs_.Materialize(1), "/c/n/f"), "f");
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  EXPECT_EQ(Read(*fs_.Materialize(0), "/c/n/f"), "f");
}

TEST_F(CrashSimFsTest, LinkedNamesShareOneFile) {
  Write(fs_, "/tmp/lock.1", "holder", /*sync=*/false);
  ASSERT_TRUE(fs_.Link("/tmp/lock.1", "/c/lock").ok());
  ASSERT_TRUE(fs_.SyncDir("/tmp").ok());
  ASSERT_TRUE(fs_.SyncDir("/c").ok());
  for (uint64_t seed = 0; seed < kSeeds; ++seed) {
    auto disk = fs_.Materialize(seed);
    EXPECT_EQ(Read(*disk, "/tmp/lock.1"), Read(*disk, "/c/lock")) << "seed " << seed;
  }
}

TEST_F(CrashSimFsTest, EveryCallAfterTheCrashPointFails) {
  const uint64_t before = fs_.mutating_calls();  // the SetUp calls
  fs_.CrashAfter(before + 2);
  ASSERT_TRUE(fs_.CreateDir("/c/d").ok());
  auto f = fs_.CreateNew("/c/f");
  ASSERT_TRUE(f.ok());
  EXPECT_FALSE(fs_.crashed());
  EXPECT_EQ((*f)->Append(AsBytes("x")).code(), StatusCode::kIoError);
  EXPECT_TRUE(fs_.crashed());
  EXPECT_EQ((*f)->Close().code(), StatusCode::kIoError);
  EXPECT_EQ(fs_.ListDir("/c").status().code(), StatusCode::kIoError);
  EXPECT_EQ(fs_.SyncDir("/c").code(), StatusCode::kIoError);
  EXPECT_EQ(fs_.mutating_calls(), before + 4);
}

}  // namespace
}  // namespace dds
