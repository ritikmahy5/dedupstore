// Repository versus a reference model (TESTING.md 3.5). A seeded random sequence of
// backup, rm, restore, ls, verify, and close/reopen runs against a MemFs repository and against a
// model: a map from snapshot id to the tree that snapshot must restore to. After every step, every
// live snapshot restores to exactly its tree and verify passes. GC runs at random thresholds,
// sometimes as a dry run, and must never change what any live snapshot restores to.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>

#include "dds/gc/gc_runner.h"
#include "dds/repo/repository.h"
#include "support/repo_fixture.h"
#include "support/test_seed.h"
#include "support/tree_compare.h"
#include "support/tree_generator.h"

namespace dds {
namespace {

using testing::TreeGenerator;
using testing::TreeGenOptions;

class RepositoryModelTest : public testing::RepoTest {
 protected:
  void Run(uint64_t seed, size_t steps, TreeGenOptions options);

 private:
  void Backup(TreeGenerator& gen, bool mutate);
  void RemoveOne();
  void ListOne();
  void Gc();
  // Every live snapshot restores to its model tree, the listing matches, and verify passes.
  void CheckAll(bool deep);
  SnapshotId PickLive() {
    auto it = model_.begin();
    std::advance(it, static_cast<ptrdiff_t>(rng_() % model_.size()));
    return it->first;
  }

  std::mt19937_64 rng_;
  std::map<SnapshotId, MemTree> model_;
  MemTree current_;
  std::optional<SnapshotId> last_;  // the snapshot of current_, if current_ has not changed since
};

void RepositoryModelTest::Run(uint64_t seed, size_t steps, TreeGenOptions options) {
  // Each seed starts from no snapshots; blobs left by an earlier seed are only unreachable space.
  auto existing = repo_->ListSnapshots();
  ASSERT_TRUE(existing.ok());
  for (const SnapshotInfo& info : *existing) ASSERT_TRUE(repo_->RemoveSnapshot(info.id).ok());
  model_.clear();
  last_.reset();
  rng_.seed(seed);
  TreeGenerator gen(rng_(), options);
  current_ = gen.NewTree();
  for (size_t step = 0; step < steps; ++step) {
    SCOPED_TRACE(::testing::Message() << "step " << step);
    const uint64_t op = rng_() % 12;
    if (op <= 3 || model_.empty()) {
      Backup(gen, /*mutate=*/rng_() % 4 != 0);
    } else if (op <= 5) {
      RemoveOne();
    } else if (op <= 7) {
      ListOne();
    } else if (op == 8) {
      Reopen();
    } else if (op == 9) {
      Gc();
    } else if (op == 10) {
      Gc();
      RemoveOne();
    } else {
      // A backup of the same tree with no change: zero new blobs.
      Backup(gen, /*mutate=*/false);
    }
    CheckAll(/*deep=*/step % 5 == 0);
    if (HasFatalFailure() || HasNonfatalFailure()) return;  // one clear failure, not a cascade
  }
}

void RepositoryModelTest::Backup(TreeGenerator& gen, bool mutate) {
  if (mutate) {
    gen.Mutate(current_);
    last_.reset();
  }
  MemSourceTree source(current_);
  const BackupResult b = BackupOk(source);
  if (last_.has_value() && model_.count(*last_) != 0) {
    EXPECT_EQ(b.ingest.blobs_written, 0u) << "unchanged tree wrote new blobs";
    EXPECT_EQ(b.manifest.root, repo_->Internals().snapshots.Read(*last_)->root);
  }
  uint64_t files = 0, dirs = 0, links = 0, bytes = 0;
  for (const auto& [path, node] : current_) {
    files += node.kind == EntryKind::kFile;
    dirs += node.kind == EntryKind::kDirectory;
    links += node.kind == EntryKind::kSymlink;
    if (node.kind == EntryKind::kFile) bytes += node.content.size();
  }
  EXPECT_EQ(b.manifest.file_count, files);
  EXPECT_EQ(b.manifest.directory_count, dirs);
  EXPECT_EQ(b.manifest.symlink_count, links);
  EXPECT_EQ(b.manifest.logical_bytes, bytes);
  model_[b.id] = current_;
  last_ = b.id;
}

void RepositoryModelTest::RemoveOne() {
  if (model_.empty()) return;
  const SnapshotId id = PickLive();
  ASSERT_TRUE(repo_->RemoveSnapshot(id).ok());
  model_.erase(id);
}

void RepositoryModelTest::Gc() {
  constexpr double kThresholds[] = {0.3, 0.7, 1.0};
  GcOptions options;
  options.threshold = kThresholds[rng_() % 3];
  options.dry_run = rng_() % 4 == 0;
  GcRunner gc(*repo_);
  auto report = gc.Run(options);
  ASSERT_TRUE(report.ok()) << report.status().ToString();
}

void RepositoryModelTest::ListOne() {
  const SnapshotId id = PickLive();
  const MemTree& tree = model_.at(id);
  std::vector<std::string> dirs = {""};
  for (const auto& [path, node] : tree) {
    if (node.kind == EntryKind::kDirectory) dirs.push_back(path);
  }
  const std::string dir = dirs[rng_() % dirs.size()];
  auto listed = repo_->ListPath(id, dir);
  ASSERT_TRUE(listed.ok()) << dir << ": " << listed.status().ToString();
  std::vector<std::string> expected;
  for (const auto& [path, node] : tree) {
    const std::string prefix = dir.empty() ? "" : dir + "/";
    if (path.starts_with(prefix) && path.find('/', prefix.size()) == std::string::npos) {
      expected.push_back(path.substr(prefix.size()));
    }
  }
  std::sort(expected.begin(), expected.end());
  std::vector<std::string> names;
  for (const TreeEntry& e : *listed) names.push_back(e.name);
  EXPECT_EQ(names, expected) << "ls " << dir;
}

void RepositoryModelTest::CheckAll(bool deep) {
  auto listed = repo_->ListSnapshots();
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed->size(), model_.size());
  for (const SnapshotInfo& info : *listed) ASSERT_EQ(model_.count(info.id), 1u);
  for (const auto& [id, tree] : model_) {
    const auto diffs = testing::DiffMemTrees(tree, RestoreOk(id));
    ASSERT_TRUE(diffs.empty()) << SnapshotIdToHex(id) << ": " << ::testing::PrintToString(diffs);
  }
  VerifyOptions options;
  options.deep = deep;
  auto report = repo_->Verify(options);
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  EXPECT_TRUE(report->ok()) << ::testing::PrintToString(report->errors);
}

TEST_F(RepositoryModelTest, RandomOperationsMatchTheModel) {
  TreeGenOptions options;
  options.files = 12;
  options.max_file_size = 128 << 10;
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    Run(seed, 20, options);
    if (HasFailure()) return;
  }
}

// The same with zstd (D107) and compressible content, so the repository holds compressed and raw
// blobs side by side through every operation, GC copies included.
class CompressedRepositoryModelTest : public RepositoryModelTest {
 protected:
  void SetUp() override {
    RepoParams params;
    params.compression = {Codec::kZstd, 3};
    ASSERT_TRUE(Repository::Init(fs_, "/r", params).ok());
    Reopen();
  }
};

TEST_F(CompressedRepositoryModelTest, RandomOperationsMatchTheModel) {
  TreeGenOptions options;
  options.files = 12;
  options.max_file_size = 128 << 10;
  options.compressible = true;
  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    Run(seed, 20, options);
    if (HasFailure()) return;
  }
  // Both kinds of blob occurred.
  size_t compressed = 0, raw = 0;
  for (size_t s = 0; s < ChunkIndex::kShardCount; ++s) {
    repo_->Internals().index.ForEachInShard(
        s, [&](const IndexEntry& e) { ++(e.codec == Codec::kZstd ? compressed : raw); });
  }
  EXPECT_GT(compressed, 10u);
  EXPECT_GT(raw, 10u);
}

class RepositoryModelSlowTest : public RepositoryModelTest {};

TEST_F(RepositoryModelSlowTest, LongerRunsWithLargeFiles) {
  TreeGenOptions options;
  options.files = 30;
  options.max_file_size = 3 << 20;
  for (uint64_t seed : testing::TestSeeds(4)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    Run(seed, 40, options);
    if (HasFailure()) return;
  }
}

}  // namespace
}  // namespace dds
