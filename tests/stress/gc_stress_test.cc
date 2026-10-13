// Ingest + restore + rm + GC at once (TESTING.md 3.7; G6): 4 backup threads on trees
// sharing most of their content, splitting files of 64 KiB and more across their workers (D109), 2
// restorers checking random committed snapshots against the model, 1 thread removing old snapshots,
// 1 thread running GC in a loop at threshold 0.9 for the most churn, and 1 thread writing index
// checkpoints until GC stops (D104). Fixed operation counts, not durations. A restore may fail only
// because its snapshot was removed meanwhile (Aborted, or NotFound if it was gone before it began).
// At the end, every snapshot not removed restores exactly and verify passes, both in the running
// repository and after reopening it from the last checkpoint. This looks for the races nobody wrote
// a deterministic test for.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/fs/mem_fs.h"
#include "dds/gc/gc_runner.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/restore_sink.h"
#include "dds/snapshot/source_tree.h"
#include "support/fake_process_identity.h"
#include "support/test_seed.h"
#include "support/tree_compare.h"
#include "support/tree_generator.h"

namespace dds {
namespace {

using testing::TreeGenerator;
using testing::TreeGenOptions;

// Committed snapshots, and those removed (marked before the removal starts, so a restore that
// fails because of it can always be explained).
class Model {
 public:
  void Add(const SnapshotId& id, MemTree tree) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    live_[id] = std::move(tree);
  }
  std::optional<std::pair<SnapshotId, MemTree>> Pick(std::mt19937_64& rng) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    if (live_.empty()) return std::nullopt;
    auto it = live_.begin();
    std::advance(it, static_cast<ptrdiff_t>(rng() % live_.size()));
    return *it;
  }
  // Moves a random live snapshot to the removed set, keeping at least `keep` live.
  std::optional<SnapshotId> StartRemoving(std::mt19937_64& rng, size_t keep) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    if (live_.size() <= keep) return std::nullopt;
    auto it = live_.begin();
    std::advance(it, static_cast<ptrdiff_t>(rng() % live_.size()));
    const SnapshotId id = it->first;
    removed_.insert(id);
    live_.erase(it);
    return id;
  }
  bool Removed(const SnapshotId& id) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    return removed_.count(id) != 0;
  }
  std::map<SnapshotId, MemTree> Live() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    return live_;
  }

 private:
  OrderedMutex mu_{lock_level::kLeaf, "test.model"};
  std::map<SnapshotId, MemTree> live_ DDS_GUARDED_BY(mu_);
  std::set<SnapshotId> removed_ DDS_GUARDED_BY(mu_);
};

// Counts finished operations so the GC thread can run once per operation. WaitForNext returns true
// after each new operation and false once every producer is done and all were seen.
class Events {
 public:
  void Happened() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    ++happened_;
    cv_.notify_all();
  }
  void ProducerDone() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    --producers_;
    cv_.notify_all();
  }
  bool WaitForNext() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (seen_ == happened_ && producers_ > 0) cv_.wait(lock);
    if (seen_ == happened_) return false;
    ++seen_;
    return true;
  }

 private:
  OrderedMutex mu_{lock_level::kLeaf, "test.events"};
  std::condition_variable_any cv_;         // waited on with mu_
  int producers_ DDS_GUARDED_BY(mu_) = 5;  // 4 backup threads and the rm thread
  uint64_t happened_ DDS_GUARDED_BY(mu_) = 0;
  uint64_t seen_ DDS_GUARDED_BY(mu_) = 0;
};

struct Sizes {
  size_t backups_per_thread;
  size_t restores_per_thread;
  size_t removals;
  size_t gc_runs;
  TreeGenOptions tree;
};

// Every snapshot the model holds restores exactly, and verify --deep finds nothing.
void ExpectModelHolds(Repository& repo, Model& model) {
  for (const auto& [id, tree] : model.Live()) {
    MemRestoreSink sink;
    auto restored = repo.Restore(id, sink, RestoreOptions{});
    ASSERT_TRUE(restored.ok()) << SnapshotIdToHex(id) << ": " << restored.status().ToString();
    EXPECT_TRUE(testing::DiffMemTrees(tree, sink.nodes()).empty()) << SnapshotIdToHex(id);
  }
  VerifyOptions deep;
  deep.deep = true;
  auto report = repo.Verify(deep);
  ASSERT_TRUE(report.ok());
  EXPECT_TRUE(report->ok()) << ::testing::PrintToString(report->errors);
}

void RunStress(uint64_t seed, const Sizes& sizes) {
  MemFs fs;
  ASSERT_TRUE(Repository::Init(fs, "/r", RepoParams{}).ok());
  testing::FakeProcessIdentity self("h", 1, 1);
  auto opened = Repository::Open(fs, "/r", self);
  ASSERT_TRUE(opened.ok());
  Repository& repo = **opened;
  Model model;
  const MemTree base = TreeGenerator(seed, sizes.tree).NewTree();

  // GC runs once after each completed backup or removal (and gc_runs times at least), so its runs
  // overlap the other operations however the threads are scheduled, and their number is bounded by
  // the operations, not by time.
  Events events;
  std::vector<std::thread> threads;
  for (uint64_t b = 0; b < 4; ++b) {
    threads.emplace_back([&, b] {
      TreeGenerator gen(seed + 1 + b, sizes.tree);
      MemTree tree = base;
      for (size_t i = 0; i < sizes.backups_per_thread; ++i) {
        gen.Mutate(tree);
        MemSourceTree source(tree);
        BackupOptions options;
        options.threads = 2;
        options.split_file_bytes = 64 << 10;  // the larger files go through segments (D109)
        auto result = repo.Backup(source, options);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        model.Add(result->id, tree);
        events.Happened();
      }
      events.ProducerDone();
    });
  }
  for (uint64_t t = 0; t < 2; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(seed * 31 + t);
      for (size_t i = 0; i < sizes.restores_per_thread; ++i) {
        auto pick = model.Pick(rng);
        if (!pick.has_value()) continue;
        MemRestoreSink sink;
        auto restored = repo.Restore(pick->first, sink, RestoreOptions{});
        if (!restored.ok()) {
          const StatusCode c = restored.status().code();
          EXPECT_TRUE((c == StatusCode::kAborted || c == StatusCode::kNotFound) &&
                      model.Removed(pick->first))
              << restored.status().ToString();
          continue;
        }
        const auto diffs = testing::DiffMemTrees(pick->second, sink.nodes());
        EXPECT_TRUE(diffs.empty()) << ::testing::PrintToString(diffs);
      }
    });
  }
  threads.emplace_back([&] {
    std::mt19937_64 rng(seed * 37);
    for (size_t i = 0; i < sizes.removals; ++i) {
      auto id = model.StartRemoving(rng, /*keep=*/2);
      if (id.has_value()) {
        EXPECT_TRUE(repo.RemoveSnapshot(*id).ok());
      }
      events.Happened();
    }
    events.ProducerDone();
  });
  // Set when the GC thread stops, however it stops, so the checkpoint thread always ends.
  std::atomic<bool> gc_done{false};
  struct SetOnExit {
    std::atomic<bool>& flag;
    ~SetOnExit() { flag.store(true); }
  };
  threads.emplace_back([&] {
    const SetOnExit done{gc_done};
    GcRunner gc(repo);
    GcOptions options;
    options.threshold = 0.9;
    size_t runs = 0;
    while (events.WaitForNext() || runs < sizes.gc_runs) {
      auto report = gc.Run(options);
      ASSERT_TRUE(report.ok()) << report.status().ToString();
      ++runs;
    }
    std::printf("[stress] %zu gc runs\n", runs);
  });
  threads.emplace_back([&] {
    size_t written = 0;
    do {
      ASSERT_TRUE(repo.WriteIndexCheckpoint().ok());
      ++written;
    } while (!gc_done.load());
    std::printf("[stress] %zu index checkpoints\n", written);
  });
  for (std::thread& t : threads) t.join();
  ExpectModelHolds(repo, model);

  opened->reset();  // closes it, so it can be opened again
  auto reopened = Repository::Open(fs, "/r", self);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_TRUE((*reopened)->open_report().checkpoint_rejected.empty())
      << (*reopened)->open_report().checkpoint_rejected;
  ExpectModelHolds(**reopened, model);
}

TEST(GcStressTest, IngestRestoreRmAndLoopingGc) {
  Sizes sizes{4, 8, 6, 6, {}};
  sizes.tree.files = 12;
  sizes.tree.max_file_size = 64 << 10;
  RunStress(testing::TestSeed(), sizes);
}

class GcStressSlowTest : public ::testing::Test {};
TEST_F(GcStressSlowTest, LongerRuns) {
  Sizes sizes{12, 40, 30, 30, {}};
  sizes.tree.files = 30;
  sizes.tree.max_file_size = 512 << 10;
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    RunStress(seed, sizes);
  }
}

}  // namespace
}  // namespace dds
