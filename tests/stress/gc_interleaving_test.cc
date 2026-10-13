// The races ARCHITECTURE.md section 6 prevents, and reuse from a parent snapshot (D106), each
// forced deterministically with sync points (TESTING.md 3.7): every test passes with the rule in
// place and fails every time with the rule's mutation switched on, so the tests are known to be
// able to fail.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <thread>

#include "dds/common/ordered_mutex.h"
#include "dds/common/test_hooks.h"
#include "dds/gc/gc_runner.h"
#include "support/random_data.h"
#include "support/repo_fixture.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using test_hooks::ScopedMutation;
using test_hooks::ScopedSyncPoint;
using testing::RandomBytes;

// Holds one thread at a sync point until the test releases it.
class Gate {
 public:
  void ArriveAndWait() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    arrived_ = true;
    cv_.notify_all();
    while (!released_) cv_.wait(lock);
  }
  void WaitForArrival() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (!arrived_) cv_.wait(lock);
  }
  void Release() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    released_ = true;
    cv_.notify_all();
  }

 private:
  OrderedMutex mu_{lock_level::kLeaf, "test.gate"};
  std::condition_variable_any cv_;  // waited on with mu_
  bool arrived_ DDS_GUARDED_BY(mu_) = false;
  bool released_ DDS_GUARDED_BY(mu_) = false;
};

class GcInterleavingTest : public testing::RepoTest {
 protected:
  void SetUp() override {
    if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
    RepoTest::SetUp();
  }
  GcReport RunGc() {
    GcRunner gc(*repo_);
    GcOptions options;
    options.threshold = 1.0;
    auto report = gc.Run(options);
    EXPECT_TRUE(report.ok()) << report.status().ToString();
    return report.ok() ? *report : GcReport{};
  }
  // True if snapshot `id` restores to exactly `tree`.
  bool Restores(const SnapshotId& id, const MemTree& tree) {
    MemRestoreSink sink;
    auto restored = repo_->Restore(id, sink, RestoreOptions{});
    return restored.ok() && testing::DiffMemTrees(tree, sink.nodes()).empty();
  }

  // Dedup during GC (6.2 items 3-4): GC plans a snapshot's blobs dead; while it is paused at
  // gc.after_plan a new backup of the same tree hits every one of those entries and commits; GC
  // resumes. The stamps must protect the entries. If `stale_stamp`, an older epoch is then applied
  // to every entry, as a delayed stamp would be: taking the max must keep last_ref where it was.
  bool DedupDuringGc(bool stale_stamp) {
    MemSourceTree tree;
    tree.AddFile("a", RandomBytes(1, 30000));
    tree.AddFile("b", RandomBytes(2, 20000));
    const BackupResult first = BackupOk(tree);
    EXPECT_TRUE(repo_->RemoveSnapshot(first.id).ok());  // every blob is now garbage
    Gate gate;
    ScopedSyncPoint pause("gc.after_plan", [&gate] { gate.ArriveAndWait(); });
    std::thread gc([this] { RunGc(); });
    gate.WaitForArrival();
    const BackupResult again = BackupOk(tree);
    EXPECT_EQ(again.ingest.blobs_written, 0u) << "the backup must reuse the planned-dead blobs";
    if (stale_stamp) {
      RepoInternals& in = repo_->Internals();
      for (size_t shard = 0; shard < ChunkIndex::kShardCount; ++shard) {
        std::vector<Fingerprint> fps;
        in.index.ForEachInShard(shard, [&](const IndexEntry& e) { fps.push_back(e.fp); });
        for (const Fingerprint& fp : fps) in.index.LookupAndStamp(fp, /*now=*/1);
      }
    }
    gate.Release();
    gc.join();
    return Restores(again.id, tree.nodes());
  }

  // A sealed, unpublished container (6.2 item 5, D49): a backup is paused at seal.before_publish
  // while a whole GC runs; then it publishes and commits.
  bool SealedButUnpublished() {
    MemSourceTree tree;
    tree.AddFile("a", RandomBytes(3, 40000));
    Gate gate;
    std::atomic<bool> paused{false};
    ScopedSyncPoint pause("seal.before_publish", [&] {
      if (!paused.exchange(true)) gate.ArriveAndWait();  // only the first seal waits
    });
    std::optional<BackupResult> result;
    std::thread backup([&] { result = BackupOk(tree); });
    gate.WaitForArrival();
    RunGc();
    gate.Release();
    backup.join();
    return result.has_value() && Restores(result->id, tree.nodes());
  }

  // A backup with a parent (6.2 item 3, D106): it is paused at backup.after_reuse, once it has
  // looked up a reused file's recipe and chunks; the parent is removed and a whole GC runs; the
  // backup resumes and commits. The lookups' stamps must keep the reused blobs, which only the
  // parent referenced.
  bool ReuseDuringGc() {
    MemSourceTree tree;
    tree.AddFile("a", RandomBytes(6, 30000), 0644, /*sec=*/100);
    const BackupResult parent = BackupOk(tree);
    RunGc();  // advances the epoch, so the next backup starts after the parent's stamps
    Gate gate;
    ScopedSyncPoint pause("backup.after_reuse", [&gate] { gate.ArriveAndWait(); });
    std::optional<BackupResult> child;
    std::thread backup([&] {
      clock_.Advance(1000);
      BackupOptions options;
      options.parent = parent.id;
      auto result = repo_->Backup(tree, options);
      if (result.ok()) child = *result;
    });
    gate.WaitForArrival();
    EXPECT_TRUE(repo_->RemoveSnapshot(parent.id).ok());
    RunGc();
    gate.Release();
    backup.join();
    return child.has_value() && child->files_reused == 1 && Restores(child->id, tree.nodes());
  }

  // A reader during retirement (6.3, D21): a restore looks up a blob and is paused before reading
  // it; GC relocates that blob and retires its container; the restore resumes and reads at the
  // location it looked up.
  bool ReaderDuringRetire() {
    MemSourceTree both;
    both.AddFile("a", RandomBytes(4, 30000));
    MemSourceTree first_only = both;
    first_only.AddFile("zz", RandomBytes(5, 30000));  // garbage once the first snapshot is gone
    const BackupResult first = BackupOk(first_only);
    const BackupResult second = BackupOk(both);
    EXPECT_TRUE(repo_->RemoveSnapshot(first.id).ok());
    // The second snapshot's restore reads its root tree (its own, fully live container), then file
    // a's recipe, which lives in the first backup's file container next to zz: GC compacts that
    // container, moving the recipe. Pause at that second read.
    Gate gate;
    std::atomic<int> reads{0};
    ScopedSyncPoint pause("read.after_lookup", [&] {
      if (++reads == 2) gate.ArriveAndWait();
    });
    bool restored = false;
    std::thread reader([&] { restored = Restores(second.id, both.nodes()); });
    gate.WaitForArrival();
    const GcReport r = RunGc();
    EXPECT_GE(r.compact.containers_compacted, 1u);
    gate.Release();
    reader.join();
    return restored;
  }
};

TEST_F(GcInterleavingTest, DedupDuringGc) { EXPECT_TRUE(DedupDuringGc(/*stale_stamp=*/false)); }
TEST_F(GcInterleavingTest, DedupDuringGcFailsWithoutEpochProtection) {
  ScopedMutation broken("no_epoch_protection");
  EXPECT_FALSE(DedupDuringGc(/*stale_stamp=*/false));
}

TEST_F(GcInterleavingTest, StaleStampDoesNotLowerLastRef) {
  EXPECT_TRUE(DedupDuringGc(/*stale_stamp=*/true));
}
TEST_F(GcInterleavingTest, StaleStampFailsWhenStampsAssign) {
  ScopedMutation broken("stamp_assign");
  EXPECT_FALSE(DedupDuringGc(/*stale_stamp=*/true));
}

TEST_F(GcInterleavingTest, SealedButUnpublishedContainerIsNotACandidate) {
  EXPECT_TRUE(SealedButUnpublished());
}
TEST_F(GcInterleavingTest, SealedButUnpublishedFailsWhenCandidatesIncludeIt) {
  ScopedMutation broken("candidate_before_publish");
  EXPECT_FALSE(SealedButUnpublished());
}

TEST_F(GcInterleavingTest, ReusedBlobsSurviveRemovingTheParentAndGc) {
  EXPECT_TRUE(ReuseDuringGc());
}
TEST_F(GcInterleavingTest, ReuseDuringGcFailsWithoutStamps) {
  ScopedMutation broken("reuse_without_stamp");
  EXPECT_FALSE(ReuseDuringGc());
}

TEST_F(GcInterleavingTest, ReaderDuringRetireReadsTheOldContainer) {
  EXPECT_TRUE(ReaderDuringRetire());
}
TEST_F(GcInterleavingTest, ReaderDuringRetireFailsWithoutGrace) {
  ScopedMutation broken("retire_without_grace");
  EXPECT_FALSE(ReaderDuringRetire());
}

}  // namespace
}  // namespace dds
