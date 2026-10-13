// Backups with a parent snapshot (D106): which files count as unchanged, that they are
// not read, that the snapshot is the one a full read would give, and the documented limitation.

#include "dds/repo/backup_parent.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

#include "support/random_data.h"
#include "support/repo_fixture.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using testing::RandomBytes;

constexpr int64_t kSecond = 1'000'000'000;

// A source tree that counts the files a backup opens.
class CountingSourceTree final : public SourceTree {
 public:
  explicit CountingSourceTree(MemTree nodes) : inner_(std::move(nodes)) {}
  Result<std::vector<SourceEntry>> ListDir(const std::string& path) override {
    return inner_.ListDir(path);
  }
  Result<std::unique_ptr<ByteSource>> OpenFile(const std::string& path) override {
    opened_.fetch_add(1);
    return inner_.OpenFile(path);
  }
  Result<std::string> ReadLink(const std::string& path) override { return inner_.ReadLink(path); }
  int opened() const { return opened_.load(); }

 private:
  MemSourceTree inner_;
  std::atomic<int> opened_{0};
};

SourceEntry Listed(uint64_t size, int64_t sec, uint32_t nsec) {
  SourceEntry s;
  s.kind = SourceKind::kFile;
  s.size = size;
  s.mtime_sec = sec;
  s.mtime_nsec = nsec;
  return s;
}

TreeEntry Recorded(uint64_t size, int64_t sec, uint32_t nsec) {
  TreeEntry e;
  e.kind = EntryKind::kFile;
  e.size = size;
  e.mtime_sec = sec;
  e.mtime_nsec = nsec;
  return e;
}

TEST(BackupParentTest, UnchangedMeansSameKindSizeAndMtimeWellBeforeTheParentStarted) {
  const int64_t started = 1000 * kSecond;
  EXPECT_TRUE(UnchangedSinceParent(Listed(10, 500, 7), Recorded(10, 500, 7), started));
  EXPECT_FALSE(UnchangedSinceParent(Listed(11, 500, 7), Recorded(10, 500, 7), started));
  EXPECT_FALSE(UnchangedSinceParent(Listed(10, 501, 7), Recorded(10, 500, 7), started));
  EXPECT_FALSE(UnchangedSinceParent(Listed(10, 500, 8), Recorded(10, 500, 7), started));
  TreeEntry dir = Recorded(10, 500, 7);
  dir.kind = EntryKind::kDirectory;
  EXPECT_FALSE(UnchangedSinceParent(Listed(10, 500, 7), dir, started));
  // The racy window: an mtime within kParentMtimeMarginSec of the parent's start is read again.
  EXPECT_TRUE(UnchangedSinceParent(Listed(1, 997, 0), Recorded(1, 997, 0), started));
  EXPECT_FALSE(UnchangedSinceParent(Listed(1, 998, 0), Recorded(1, 998, 0), started));
  EXPECT_FALSE(UnchangedSinceParent(Listed(1, 1200, 0), Recorded(1, 1200, 0), started));
  // A version 1 parent records no start: nothing is unchanged.
  EXPECT_FALSE(UnchangedSinceParent(Listed(10, 500, 7), Recorded(10, 500, 7), 0));
  // Times before 1970 and far in the future compare without overflowing.
  EXPECT_TRUE(UnchangedSinceParent(Listed(1, -5, 0), Recorded(1, -5, 0), started));
  EXPECT_FALSE(UnchangedSinceParent(Listed(1, INT64_MAX, 0), Recorded(1, INT64_MAX, 0), started));
}

TEST(BackupParentTest, FindEntryByName) {
  Tree t;
  for (const char* name : {"a", "b", "d"}) {
    TreeEntry e;
    e.name = name;
    t.entries.push_back(e);
  }
  ASSERT_NE(FindEntry(t, "b"), nullptr);
  EXPECT_EQ(FindEntry(t, "b")->name, "b");
  EXPECT_EQ(FindEntry(t, "c"), nullptr);
  EXPECT_EQ(FindEntry(t, "e"), nullptr);
}

class BackupWithParentTest : public testing::RepoTest {
 protected:
  // Files with mtimes far older than the manual clock, so they qualify for reuse.
  static MemTree Sample() {
    MemSourceTree t;
    t.AddDir("dir");
    t.AddFile("dir/a", RandomBytes(1, 40000), 0644, 100, 1);
    t.AddFile("dir/b", RandomBytes(2, 9000), 0644, 100, 2);
    t.AddFile("empty", {}, 0644, 100, 3);
    t.AddFile("c", RandomBytes(3, 70000), 0600, 100, 4);
    t.AddSymlink("link", "dir/a", 100, 5);
    return t.nodes();
  }

  BackupResult Backup(CountingSourceTree& source, std::optional<SnapshotId> parent) {
    clock_.Advance(10 * kSecond);
    BackupOptions options;
    options.source_path = "/src";
    options.threads = 2;
    options.parent = parent;
    auto result = repo_->Backup(source, options);
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    return result.ok() ? *result : BackupResult{};
  }

  void ExpectRestores(const SnapshotId& id, const MemTree& expected) {
    const std::vector<std::string> diffs = testing::DiffMemTrees(expected, RestoreOk(id));
    EXPECT_TRUE(diffs.empty()) << diffs.front();
  }
};

TEST_F(BackupWithParentTest, AnUnchangedTreeIsNotReadAndGivesTheSameSnapshotRoot) {
  CountingSourceTree first_source(Sample());
  const BackupResult first = Backup(first_source, std::nullopt);
  EXPECT_EQ(first_source.opened(), 4);
  EXPECT_EQ(first.manifest.format_version, 2u);
  EXPECT_GT(first.manifest.started_at_ns, 0);
  EXPECT_LE(first.manifest.started_at_ns, first.manifest.created_at_ns);
  CountingSourceTree second_source(Sample());
  const BackupResult second = Backup(second_source, first.id);
  EXPECT_EQ(second_source.opened(), 0);
  EXPECT_EQ(second.files_reused, 4u);
  EXPECT_EQ(second.bytes_reused, 40000u + 9000u + 70000u);
  EXPECT_EQ(second.manifest.file_count, first.manifest.file_count);
  EXPECT_EQ(second.manifest.logical_bytes, first.manifest.logical_bytes);
  EXPECT_EQ(second.manifest.root, first.manifest.root);  // the same trees a full read gives
  EXPECT_EQ(second.ingest.blobs_written, 0u);
  ExpectRestores(second.id, Sample());
}

TEST_F(BackupWithParentTest, ChangedFilesAreReadAndTheRestAreReused) {
  CountingSourceTree first_source(Sample());
  const BackupResult first = Backup(first_source, std::nullopt);
  MemTree changed = Sample();
  changed["dir/a"].content = RandomBytes(10, 40000);  // same size, new mtime
  changed["dir/a"].mtime_sec = 200;
  changed["c"].content = RandomBytes(11, 70001);  // new size, same mtime
  changed["dir/new"] = changed["dir/b"];
  changed["dir/new"].content = RandomBytes(12, 500);
  changed.erase("empty");
  CountingSourceTree second_source(changed);
  const BackupResult second = Backup(second_source, first.id);
  EXPECT_EQ(second_source.opened(), 3);  // dir/a, c, dir/new
  EXPECT_EQ(second.files_reused, 1u);    // dir/b
  ExpectRestores(second.id, changed);
}

// SPEC.md section 4: a change that keeps the size and restores the old mtime is not noticed, the
// limitation of every mtime-based check. Without a parent the file is read.
TEST_F(BackupWithParentTest, AChangeThatKeepsSizeAndMtimeIsOnlySeenWithoutAParent) {
  CountingSourceTree first_source(Sample());
  const BackupResult first = Backup(first_source, std::nullopt);
  MemTree sneaky = Sample();
  sneaky["dir/b"].content = RandomBytes(20, 9000);
  CountingSourceTree with_parent(sneaky);
  const BackupResult reused = Backup(with_parent, first.id);
  EXPECT_EQ(with_parent.opened(), 0);
  ExpectRestores(reused.id, Sample());  // the parent's content, as documented
  CountingSourceTree without_parent(sneaky);
  const BackupResult read = Backup(without_parent, std::nullopt);
  EXPECT_EQ(without_parent.opened(), 4);
  ExpectRestores(read.id, sneaky);
}

TEST_F(BackupWithParentTest, FilesModifiedJustBeforeTheParentStartedAreReadAgain) {
  MemTree recent = Sample();
  // Within kParentMtimeMarginSec of the next backup's start (the clock advances 10 s first).
  const int64_t next_start_sec = (clock_.WallNanos() + 10 * kSecond) / kSecond;
  recent["c"].mtime_sec = next_start_sec - 1;
  CountingSourceTree first_source(recent);
  const BackupResult first = Backup(first_source, std::nullopt);
  CountingSourceTree second_source(recent);
  const BackupResult second = Backup(second_source, first.id);
  EXPECT_EQ(second_source.opened(), 1);
  EXPECT_EQ(second.files_reused, 3u);
  ExpectRestores(second.id, recent);
}

TEST_F(BackupWithParentTest, AVersion1ParentLetsNothingBeSkipped) {
  CountingSourceTree first_source(Sample());
  const BackupResult first = Backup(first_source, std::nullopt);
  Manifest v1 = first.manifest;
  v1.format_version = 1;
  v1.started_at_ns = 0;
  auto old = repo_->Internals().snapshots.Commit(EncodeManifest(v1));
  ASSERT_TRUE(old.ok());
  CountingSourceTree second_source(Sample());
  const BackupResult second = Backup(second_source, *old);
  EXPECT_EQ(second_source.opened(), 4);
  EXPECT_EQ(second.files_reused, 0u);
}

TEST_F(BackupWithParentTest, AMissingParentIsNotFound) {
  CountingSourceTree source(Sample());
  BackupOptions options;
  options.parent = SnapshotId{};
  EXPECT_EQ(repo_->Backup(source, options).status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace dds
