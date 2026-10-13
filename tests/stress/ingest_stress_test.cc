// Parallel ingest (TESTING.md 3.7): determinism across thread counts (G11),
// concurrent backups with concurrent restores, and exact duplicate accounting, through a GC that
// removes every duplicate (G5). Run under
// tsan as well as debug; the lock-order checker is on in both.

#include <gtest/gtest.h>

#include <condition_variable>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/test_hooks.h"
#include "dds/fs/mem_fs.h"
#include "dds/gc/gc_runner.h"
#include "dds/repo/repository.h"
#include "dds/store/container_format.h"
#include "dds/store/container_reader.h"
#include "support/fake_process_identity.h"
#include "support/test_seed.h"
#include "support/tree_compare.h"
#include "support/tree_generator.h"

namespace dds {
namespace {

using testing::TreeGenerator;
using testing::TreeGenOptions;

// A MemFs repository with its own clock and process identity.
struct MemRepo {
  MemRepo() {
    EXPECT_TRUE(Repository::Init(fs, "/r", RepoParams{}).ok());
    OpenOptions options;
    options.clock = &clock;
    auto opened = Repository::Open(fs, "/r", self, options);
    EXPECT_TRUE(opened.ok()) << opened.status().ToString();
    repo = std::move(opened).value();
  }
  Result<BackupResult> Backup(const MemTree& tree, size_t threads) {
    MemSourceTree source(tree);
    BackupOptions options;
    options.threads = threads;
    return repo->Backup(source, options);
  }

  MemFs fs;
  ManualClock clock{1700000000000000000, 1};
  testing::FakeProcessIdentity self{"h", 1, 1};
  std::unique_ptr<Repository> repo;
};

constexpr size_t kThreadCounts[] = {1, 2, 4, 8, 16};

void DeterminismRun(size_t files, size_t max_file_size, size_t seeds) {
  for (uint64_t seed : testing::TestSeeds(seeds)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    TreeGenOptions options;
    options.files = files;
    options.max_file_size = max_file_size;
    const MemTree tree = TreeGenerator(seed, options).NewTree();
    std::optional<BackupResult> first;
    for (const size_t threads : kThreadCounts) {
      MemRepo r;
      auto b = r.Backup(tree, threads);
      ASSERT_TRUE(b.ok()) << b.status().ToString();
      if (!first.has_value()) {
        first = *b;
        continue;
      }
      // G11: the same tree gives the same Merkle root, and the same blobs, however it was split
      // among workers.
      EXPECT_EQ(b->manifest.root, first->manifest.root) << threads << " threads";
      EXPECT_EQ(b->ingest.blobs_written, first->ingest.blobs_written) << threads << " threads";
      EXPECT_EQ(b->ingest.bytes_written, first->ingest.bytes_written) << threads << " threads";
      EXPECT_EQ(b->manifest.file_count, first->manifest.file_count);
      EXPECT_EQ(b->manifest.logical_bytes, first->manifest.logical_bytes);
    }
  }
}

TEST(DeterminismTest, SameRootForOneToSixteenThreads) { DeterminismRun(60, 128 << 10, 2); }

class DeterminismSlowTest : public ::testing::Test {};
TEST_F(DeterminismSlowTest, LargerTrees) { DeterminismRun(400, 2 << 20, 4); }

// The committed snapshots and the trees they must restore to, shared by all threads.
class Model {
 public:
  void Add(const SnapshotId& id, MemTree tree) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    trees_[id] = std::move(tree);
  }
  // A random committed snapshot and its tree (copied, so no lock is held while restoring).
  std::pair<SnapshotId, MemTree> Pick(std::mt19937_64& rng) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    auto it = trees_.begin();
    std::advance(it, static_cast<ptrdiff_t>(rng() % trees_.size()));
    return *it;
  }
  std::map<SnapshotId, MemTree> All() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    return trees_;
  }

 private:
  OrderedMutex mu_{lock_level::kLeaf, "test.model"};
  std::map<SnapshotId, MemTree> trees_ DDS_GUARDED_BY(mu_);
};

void ExpectRestores(Repository& repo, const SnapshotId& id, const MemTree& tree) {
  MemRestoreSink sink;
  auto restored = repo.Restore(id, sink, RestoreOptions{});
  ASSERT_TRUE(restored.ok()) << SnapshotIdToHex(id) << ": " << restored.status().ToString();
  const auto diffs = testing::DiffMemTrees(tree, sink.nodes());
  EXPECT_TRUE(diffs.empty()) << SnapshotIdToHex(id) << ": " << ::testing::PrintToString(diffs);
}

// Counts, straight from the container files, the footer entries and the distinct fingerprints:
// the definition of duplicate copies in D32, computed without the index.
uint64_t DuplicatesFromFooters(MemRepo& r) {
  auto names = r.fs.ListDir("/r/containers");
  EXPECT_TRUE(names.ok());
  uint64_t entries = 0;
  std::set<Fingerprint> distinct;
  for (const std::string& name : *names) {
    const auto id = ParseContainerFileName(name);
    EXPECT_TRUE(id.has_value()) << name;
    auto meta = ReadContainerMetadata(r.fs, "/r/containers/" + name, *id, r.repo->config().repo_id,
                                      r.repo->config().chunker.max_size);
    EXPECT_TRUE(meta.ok()) << meta.status().ToString();
    for (const SealedEntry& e : meta->entries) distinct.insert(e.fp);
    entries += meta->entries.size();
  }
  return entries - distinct.size();
}

void StressRun(uint64_t seed, size_t backups_per_thread, size_t restores_per_thread,
               TreeGenOptions options) {
  MemRepo r;
  Model model;
  const MemTree base = TreeGenerator(seed, options).NewTree();
  auto first = r.Backup(base, 2);
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  model.Add(first->id, base);

  std::vector<std::thread> threads;
  for (uint64_t b = 0; b < 4; ++b) {
    threads.emplace_back([&, b] {
      // Each thread evolves its own copy of the shared base, so the threads' trees share most of
      // their content and race to store the same new blobs.
      TreeGenerator gen(seed + 1 + b, options);
      MemTree tree = base;
      for (size_t i = 0; i < backups_per_thread; ++i) {
        gen.Mutate(tree);
        auto result = r.Backup(tree, 2);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        model.Add(result->id, tree);
      }
    });
  }
  for (uint64_t t = 0; t < 2; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(seed * 31 + t);
      for (size_t i = 0; i < restores_per_thread; ++i) {
        const auto [id, tree] = model.Pick(rng);
        ExpectRestores(*r.repo, id, tree);
      }
    });
  }
  for (std::thread& t : threads) t.join();

  for (const auto& [id, tree] : model.All()) ExpectRestores(*r.repo, id, tree);
  VerifyOptions deep;
  deep.deep = true;
  auto report = r.repo->Verify(deep);
  ASSERT_TRUE(report.ok());
  EXPECT_TRUE(report->ok()) << ::testing::PrintToString(report->errors);
  auto stats = r.repo->Stats();
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->duplicate_copies, DuplicatesFromFooters(r));
}

TEST(IngestStressTest, FourBackupsAndTwoRestorersAgreeWithTheModel) {
  TreeGenOptions options;
  options.files = 15;
  options.max_file_size = 96 << 10;
  StressRun(testing::TestSeed(), 4, 8, options);
}

class IngestStressSlowTest : public ::testing::Test {};
TEST_F(IngestStressSlowTest, MoreBackupsAndLargerTrees) {
  TreeGenOptions options;
  options.files = 40;
  options.max_file_size = 1 << 20;
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    StressRun(seed, 10, 30, options);
  }
}

// Waits until `n` threads have arrived.
class Barrier {
 public:
  explicit Barrier(int n) : n_(n) {}
  void ArriveAndWait() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    ++arrived_;
    cv_.notify_all();
    while (arrived_ < n_) cv_.wait(lock);
  }

 private:
  const int n_;
  OrderedMutex mu_{lock_level::kLeaf, "test.barrier"};
  std::condition_variable_any cv_;  // waited on with mu_
  int arrived_ DDS_GUARDED_BY(mu_) = 0;
};

TEST(DuplicateAccountingTest, LockstepSessionsCountDuplicatesExactly) {
  // TESTING.md 3.7 (G5, ingest part): 4 sessions back up the same new tree. Each session's first
  // seal waits at seal.before_publish until all four have sealed, so no session can find another's
  // blobs: every blob is stored 4 times, and the count is certain, not likely.
  if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  TreeGenOptions options;
  options.files = 20;
  options.max_file_size = 64 << 10;  // the whole tree fits one container per writer
  const MemTree tree = TreeGenerator(testing::TestSeed(), options).NewTree();
  MemRepo r;
  Barrier barrier(4);
  test_hooks::ScopedSyncPoint lockstep("seal.before_publish", [&barrier] {
    thread_local bool arrived = false;  // only each session's first seal waits
    if (!arrived) {
      arrived = true;
      barrier.ArriveAndWait();
    }
  });
  std::vector<std::thread> threads;
  std::vector<SnapshotId> ids(4);
  for (size_t i = 0; i < 4; ++i) {
    threads.emplace_back([&, i] {
      auto b = r.Backup(tree, 1);
      ASSERT_TRUE(b.ok()) << b.status().ToString();
      ids[i] = b->id;
    });
  }
  for (std::thread& t : threads) t.join();
  auto stats = r.repo->Stats();
  ASSERT_TRUE(stats.ok()) << stats.status().ToString();
  EXPECT_EQ(stats->duplicate_copies, DuplicatesFromFooters(r));
  EXPECT_EQ(stats->duplicate_copies, 3 * stats->blobs) << "every blob stored by all 4 sessions";
  for (const SnapshotId& id : ids) ExpectRestores(*r.repo, id, tree);
  // G5: a GC at threshold 1.0 compacts every container holding a duplicate (a duplicate is never
  // canonical, so it counts as dead) and drops each copy, leaving none.
  GcRunner gc(*r.repo);
  GcOptions full;
  full.threshold = 1.0;
  auto report = gc.Run(full);
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  EXPECT_GT(report->compact.duplicates_dropped, 0u);
  stats = r.repo->Stats();
  ASSERT_TRUE(stats.ok());
  EXPECT_EQ(stats->duplicate_copies, 0u);
  EXPECT_EQ(DuplicatesFromFooters(r), 0u);
  for (const SnapshotId& id : ids) ExpectRestores(*r.repo, id, tree);
}

}  // namespace
}  // namespace dds
