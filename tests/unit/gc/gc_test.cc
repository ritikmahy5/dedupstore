// GcRunner (ARCHITECTURE.md 4.3) in one thread: what it deletes, compacts, keeps, and never
// touches. Concurrency is in tests/stress/, crashes in tests/crash/.

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "dds/gc/gc_runner.h"
#include "dds/store/container_format.h"
#include "support/random_data.h"
#include "support/repo_fixture.h"
#include "support/sample_trees.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using testing::RandomBytes;

// Every file in the repository and its content: what "changes nothing" compares.
std::map<std::string, std::vector<std::byte>> Snapshot(MemFs& fs, const std::string& dir) {
  std::map<std::string, std::vector<std::byte>> out;
  auto names = fs.ListDir(dir);
  if (!names.ok()) return out;
  for (const std::string& name : *names) {
    const std::string path = JoinPath(dir, name);
    if (fs.ListDir(path).ok()) {
      out[path + "/"] = {};
      auto sub = Snapshot(fs, path);
      out.insert(sub.begin(), sub.end());
    } else {
      out[path] = *ReadWholeFile(fs, path);
    }
  }
  return out;
}

class GcTest : public testing::RepoTest {
 protected:
  GcReport GcOk(double threshold, bool dry_run = false) {
    GcRunner gc(*repo_);
    GcOptions options;
    options.threshold = threshold;
    options.dry_run = dry_run;
    auto report = gc.Run(options);
    EXPECT_TRUE(report.ok()) << report.status().ToString();
    return report.ok() ? *report : GcReport{};
  }
  void ExpectVerifies() {
    VerifyOptions deep;
    deep.deep = true;
    auto report = repo_->Verify(deep);
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report->ok()) << ::testing::PrintToString(report->errors);
  }
};

MemSourceTree Version(uint64_t seed) {
  MemSourceTree t;
  t.AddDir("shared");
  t.AddFile("shared/big", RandomBytes(1, 300000));  // in every version
  t.AddFile("own", RandomBytes(seed, 200000));      // different in each
  return t;
}

TEST_F(GcTest, RemovingEverySnapshotThenGcAtOneLeavesAnEmptyRepository) {
  // No data blobs remain and only empty-repository files exist.
  MemSourceTree a = Version(10);
  MemSourceTree b = Version(11);
  const BackupResult first = BackupOk(a);
  const BackupResult second = BackupOk(b);
  ASSERT_TRUE(repo_->RemoveSnapshot(first.id).ok());
  ASSERT_TRUE(repo_->RemoveSnapshot(second.id).ok());
  const GcReport r = GcOk(1.0);
  EXPECT_EQ(r.compact.containers_deleted, r.candidates);
  EXPECT_EQ(repo_->Internals().index.Size(), 0u);
  ASSERT_TRUE(repo_->Close().ok());
  const auto files = Snapshot(fs_, "/r");
  std::vector<std::string> names;
  for (const auto& [path, content] : files) names.push_back(path);
  EXPECT_EQ(names,
            (std::vector<std::string>{"/r/config", "/r/containers/", "/r/snapshots/", "/r/tmp/"}));
}

TEST_F(GcTest, CompactionKeepsWhatIsLiveAndDropsTheRest) {
  MemSourceTree a = Version(10);
  MemSourceTree b = Version(11);
  const BackupResult first = BackupOk(a);
  const BackupResult second = BackupOk(b);
  ASSERT_TRUE(repo_->RemoveSnapshot(first.id).ok());
  const GcReport r = GcOk(1.0);
  // The first backup wrote two containers, both now partly live: files (shared/big live, its own
  // file dead) and trees (its root dead, the shared/ subtree identical in both versions, so live).
  // Both are compacted; the second backup's two containers are fully live and kept.
  EXPECT_EQ(r.compact.containers_compacted, 2u);
  EXPECT_EQ(r.compact.containers_deleted, 0u);
  EXPECT_EQ(r.planned_keep, 2u);
  EXPECT_GT(r.compact.blobs_copied, 0u);
  EXPECT_GT(r.compact.blobs_removed, 0u);
  EXPECT_TRUE(testing::DiffMemTrees(b.nodes(), RestoreOk(second.id)).empty());
  ExpectVerifies();
  auto stats = repo_->Stats();
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->live_fraction_histogram[9], stats->containers) << "all garbage is gone";
  EXPECT_EQ(stats->gc.runs, 1u);
  // And it survives a reopen, which rebuilds the index from the new containers (D12).
  Reopen();
  EXPECT_TRUE(testing::DiffMemTrees(b.nodes(), RestoreOk(second.id)).empty());
  ExpectVerifies();
}

TEST_F(GcTest, ThresholdDecidesWhatIsCompacted) {
  MemSourceTree a = Version(10);
  MemSourceTree b = Version(11);
  const BackupResult first = BackupOk(a);
  BackupOk(b);
  ASSERT_TRUE(repo_->RemoveSnapshot(first.id).ok());
  // At a tiny threshold no partly live container is compacted; dead ones are still deleted.
  const GcReport low = GcOk(0.01);
  EXPECT_EQ(low.compact.containers_compacted, 0u);
  EXPECT_GE(low.planned_keep, 1u);
  const GcReport high = GcOk(1.0);
  EXPECT_GE(high.compact.containers_compacted, 1u);
}

TEST_F(GcTest, DryRunChangesNothing) {
  // The repository's file listing and bytes are identical before and after.
  MemSourceTree a = Version(10);
  MemSourceTree b = Version(11);
  const BackupResult first = BackupOk(a);
  BackupOk(b);
  ASSERT_TRUE(repo_->RemoveSnapshot(first.id).ok());
  const auto before = Snapshot(fs_, "/r");
  const size_t index_before = repo_->Internals().index.Size();
  const GcReport r = GcOk(1.0, /*dry_run=*/true);
  EXPECT_GT(r.planned_delete + r.planned_compact, 0u);
  EXPECT_EQ(r.compact.blobs_removed + r.compact.blobs_copied, 0u);
  EXPECT_EQ(Snapshot(fs_, "/r"), before);
  EXPECT_EQ(repo_->Internals().index.Size(), index_before);
  EXPECT_EQ(repo_->Internals().containers.RetiredCount(), 0u);
}

TEST_F(GcTest, AMissingReachableBlobFailsClosedAndDeletesNothing) {
  MemSourceTree a = Version(10);
  MemSourceTree b = Version(11);
  const BackupResult first = BackupOk(a);
  BackupOk(b);
  ASSERT_TRUE(repo_->RemoveSnapshot(first.id).ok());
  // Drop the index entry of the second snapshot's root tree: reachable, missing.
  const Fingerprint root =
      repo_->Internals().snapshots.Read(repo_->ListSnapshots()->at(0).id)->root;
  const Location loc = *repo_->Internals().index.Lookup(root);
  ASSERT_EQ(repo_->Internals().index.RemoveIfDead(root, loc, 99, ~Epoch{0}),
            RemoveResult::kRemoved);
  const auto before = Snapshot(fs_, "/r");
  GcRunner gc(*repo_);
  GcOptions options;
  options.threshold = 1.0;
  auto r = gc.Run(options);
  EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().ToString();
  EXPECT_NE(r.status().message().find("nothing was deleted"), std::string::npos);
  EXPECT_EQ(Snapshot(fs_, "/r"), before);
}

TEST_F(GcTest, ThresholdOutsideZeroToOneIsInvalidArgument) {
  GcRunner gc(*repo_);
  for (double bad : {0.0, -0.5, 1.5}) {
    GcOptions options;
    options.threshold = bad;
    EXPECT_EQ(gc.Run(options).status().code(), StatusCode::kInvalidArgument) << bad;
  }
}

TEST_F(GcTest, ARetiredContainerWaitsForReadersThatEnteredBeforeIt) {
  // 6.3: reclamation unlinks a retired container only once every active reader entered after the
  // retirement.
  MemSourceTree a = Version(10);
  BackupOk(a);
  RepoInternals& in = repo_->Internals();
  const uint32_t id = in.containers.PublishedNotRetired().front();
  const std::string path = in.containers.PathFor(id);
  {
    ReadGuard early(in.epochs);
    in.containers.Retire(id, in.epochs.Advance());
    ReadGuard late(in.epochs);  // entered after the retirement: does not block it
    auto r = in.containers.ReclaimRetired(in.epochs.OldestReader());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->containers, 0u);
    EXPECT_TRUE(*fs_.Exists(path));
  }
  auto r = in.containers.ReclaimRetired(in.epochs.OldestReader());
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->containers, 1u);
  EXPECT_FALSE(*fs_.Exists(path));
}

}  // namespace
}  // namespace dds
