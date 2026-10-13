// The index checkpoint (D104): open takes covered containers' entries from it, reads
// every other container's footer, drops covered containers GC deleted, removes a checkpoint that
// fails a check, and never reuses a container id the checkpoint covers. Each test compares the
// index with the one a rebuild from every footer gives.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "dds/common/test_hooks.h"
#include "dds/gc/gc_runner.h"
#include "dds/repo/index_checkpoint_format.h"
#include "support/random_data.h"
#include "support/repo_fixture.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using testing::RandomBytes;

// Files of about 600 KiB, so each backup fills several 1 MiB containers.
MemTree Files(uint64_t seed, int count) {
  MemSourceTree t;
  for (int i = 0; i < count; ++i) {
    t.AddFile("f" + std::to_string(i), RandomBytes(seed * 100 + static_cast<uint64_t>(i), 600000));
  }
  return t.nodes();
}

class IndexCheckpointTest : public testing::RepoTest {
 protected:
  void SetUp() override {
    RepoParams params;
    params.container_target = kMinContainerTarget;
    ASSERT_TRUE(Repository::Init(fs_, "/r", params).ok());
    OpenWith(true, false);
  }

  void OpenWith(bool use, bool write, size_t stale = 256) {
    repo_.reset();
    OpenOptions options;
    options.clock = &clock_;
    options.use_index_checkpoint = use;
    options.write_index_checkpoint = write;
    options.checkpoint_stale_containers = stale;
    auto repo = Repository::Open(fs_, "/r", me_, options);
    ASSERT_TRUE(repo.ok()) << repo.status().ToString();
    repo_ = std::move(repo).value();
  }

  SnapshotId Backup(const MemTree& tree) {
    MemSourceTree source(tree);
    return BackupOk(source).id;
  }

  std::map<Fingerprint, Location> IndexMap() {
    std::map<Fingerprint, Location> m;
    for (size_t s = 0; s < ChunkIndex::kShardCount; ++s) {
      repo_->Internals().index.ForEachInShard(s,
                                              [&](const IndexEntry& e) { m[e.fp] = e.location(); });
    }
    return m;
  }

  // The index a rebuild from every footer gives, leaving the checkpoint in place.
  std::map<Fingerprint, Location> FooterIndex() {
    OpenWith(false, false);
    return IndexMap();
  }

  void ExpectVerifies() {
    VerifyOptions options;
    options.deep = true;
    auto report = repo_->Verify(options);
    ASSERT_TRUE(report.ok()) << report.status().ToString();
    EXPECT_TRUE(report->ok()) << report->errors.front();
  }

  bool CheckpointExists() { return *fs_.Exists("/r/index.ckpt"); }

  void ExpectRestores(const SnapshotId& id, const MemTree& expected) {
    const std::vector<std::string> diffs = testing::DiffMemTrees(expected, RestoreOk(id));
    EXPECT_TRUE(diffs.empty()) << diffs.front();
  }

  // GC deletes the newest containers, which the checkpoint covers. A container written afterwards
  // must not take one of their ids, or the next open would trust the checkpoint's entries for that
  // id instead of reading the new container's footer. True if the new data restores and verify
  // finds nothing.
  bool NewDataAfterGcStaysFindable(bool ignore_next_id) {
    Backup(Files(1, 2));
    const SnapshotId unrelated = Backup(Files(2, 2));
    const uint32_t newest = repo_->Internals().containers.NextId() - 1;
    EXPECT_TRUE(repo_->WriteIndexCheckpoint().ok());
    EXPECT_TRUE(repo_->RemoveSnapshot(unrelated).ok());
    GcRunner gc(*repo_);
    GcOptions options;
    options.threshold = 1.0;
    EXPECT_TRUE(gc.Run(options).ok());
    EXPECT_FALSE(*fs_.Exists(repo_->Internals().containers.PathFor(newest)));
    if (ignore_next_id) {
      test_hooks::ScopedMutation broken("checkpoint_next_id_ignored");
      OpenWith(true, false);
    } else {
      OpenWith(true, false);
      EXPECT_GT(repo_->Internals().containers.NextId(), newest);
    }
    const MemTree c = Files(3, 2);
    const SnapshotId ic = Backup(c);
    OpenWith(true, false);
    auto report = repo_->Verify(VerifyOptions{});
    MemRestoreSink sink;
    const bool restored = repo_->Restore(ic, sink, RestoreOptions{}).ok() &&
                          testing::DiffMemTrees(c, sink.nodes()).empty();
    return report.ok() && report->ok() && restored;
  }
};

TEST_F(IndexCheckpointTest, OpenTakesEveryEntryFromTheCheckpoint) {
  const MemTree a = Files(1, 4), b = Files(2, 3);
  const SnapshotId ia = Backup(a), ib = Backup(b);
  const auto before = IndexMap();
  ASSERT_TRUE(repo_->WriteIndexCheckpoint().ok());
  OpenWith(true, false);
  const OpenReport& r = repo_->open_report();
  EXPECT_TRUE(r.checkpoint_rejected.empty()) << r.checkpoint_rejected;
  EXPECT_GE(r.checkpoint_containers, 6u);
  EXPECT_EQ(r.checkpoint_containers, r.containers);
  EXPECT_EQ(r.footers_read, 0u);
  EXPECT_EQ(IndexMap(), before);
  ExpectRestores(ia, a);
  ExpectRestores(ib, b);
  ExpectVerifies();
  EXPECT_EQ(FooterIndex(), before);
}

TEST_F(IndexCheckpointTest, ContainersAfterTheCheckpointComeFromTheirFooters) {
  const MemTree a = Files(1, 3), b = Files(2, 3);
  const SnapshotId ia = Backup(a);
  ASSERT_TRUE(repo_->WriteIndexCheckpoint().ok());
  const uint64_t covered = repo_->Internals().containers.PublishedTotals().containers;
  const SnapshotId ib = Backup(b);
  const uint64_t total = repo_->Internals().containers.PublishedTotals().containers;
  OpenWith(true, false);
  EXPECT_EQ(repo_->open_report().checkpoint_containers, covered);
  EXPECT_EQ(repo_->open_report().footers_read, total - covered);
  const auto from_checkpoint = IndexMap();
  ExpectRestores(ia, a);
  ExpectRestores(ib, b);
  ExpectVerifies();
  EXPECT_EQ(FooterIndex(), from_checkpoint);
}

TEST_F(IndexCheckpointTest, ContainersGcDeletedAfterTheCheckpointAreDropped) {
  MemTree a = Files(1, 3);
  MemTree b = a;  // shares most of a, so a's containers stay partly live once a is removed
  b["f0"].content = RandomBytes(50, 600000);
  const SnapshotId ia = Backup(a), ib = Backup(b), iu = Backup(Files(3, 3));
  ASSERT_TRUE(repo_->WriteIndexCheckpoint().ok());
  ASSERT_TRUE(repo_->RemoveSnapshot(ia).ok());
  ASSERT_TRUE(repo_->RemoveSnapshot(iu).ok());
  GcRunner gc(*repo_);
  GcOptions options;
  options.threshold = 1.0;
  auto report = gc.Run(options);
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  ASSERT_GE(report->compact.containers_deleted, 1u);
  OpenWith(true, false);
  EXPECT_GT(repo_->open_report().footers_read, 0u);  // the copies GC made
  const auto from_checkpoint = IndexMap();
  ExpectRestores(ib, b);
  ExpectVerifies();
  // A rebuild from footers may also index dead copies left in surviving containers; every entry
  // the checkpoint gave must be the same there.
  const auto footers = FooterIndex();
  for (const auto& [fp, loc] : from_checkpoint) {
    ASSERT_EQ(footers.count(fp), 1u) << fp.ToHex();
    EXPECT_EQ(footers.at(fp), loc) << fp.ToHex();
  }
}

// A damaged table or trailer is found by every open; a damaged entry only by an open that loads the
// entries (with the checkpoint off, open reads only the tables, for the next container id).
TEST_F(IndexCheckpointTest, ACheckpointThatFailsACheckIsRemoved) {
  const MemTree a = Files(1, 3);
  const SnapshotId ia = Backup(a);
  const auto before = IndexMap();
  struct Case {
    bool use;
    bool damage_entries;  // else the shard table
    bool rejected;
  };
  for (const Case c : {Case{true, true, true}, Case{true, false, true}, Case{false, false, true},
                       Case{false, true, false}}) {
    SCOPED_TRACE(::testing::Message() << "use " << c.use << ", entries " << c.damage_entries);
    ASSERT_TRUE(repo_->WriteIndexCheckpoint().ok());
    auto bytes = ReadWholeFile(fs_, "/r/index.ckpt");
    ASSERT_TRUE(bytes.ok());
    const size_t at = c.damage_entries ? 100 : bytes->size() - kCheckpointTrailerSize - 8;
    (*bytes)[at] ^= std::byte{0x40};
    ASSERT_TRUE(fs_.Remove("/r/index.ckpt").ok());
    auto file = fs_.CreateNew("/r/index.ckpt");
    ASSERT_TRUE(file.ok() && (*file)->Append(*bytes).ok() && (*file)->Close().ok());
    OpenWith(c.use, false);
    EXPECT_EQ(repo_->open_report().checkpoint_rejected.empty(), !c.rejected);
    EXPECT_EQ(CheckpointExists(), !c.rejected);
    EXPECT_EQ(repo_->open_report().checkpoint_containers, 0u);
    EXPECT_EQ(IndexMap(), before);
    ExpectRestores(ia, a);
    ExpectVerifies();
  }
}

TEST_F(IndexCheckpointTest, NoContainerIdTheCheckpointCoversIsReused) {
  EXPECT_TRUE(NewDataAfterGcStaysFindable(/*ignore_next_id=*/false));
}

TEST_F(IndexCheckpointTest, IgnoringTheCheckpointsNextIdLosesNewData) {
  if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  EXPECT_FALSE(NewDataAfterGcStaysFindable(/*ignore_next_id=*/true));
}

// D107: in a compressed repository the checkpoint keeps each entry's codec, so blobs loaded from it
// still decompress.
TEST(IndexCheckpointCompressionTest, EntriesKeepTheirCodec) {
  MemFs fs;
  RepoParams params;
  params.container_target = kMinContainerTarget;
  params.compression = {Codec::kZstd, 3};
  ASSERT_TRUE(Repository::Init(fs, "/r", params).ok());
  testing::FakeProcessIdentity me("h", 1, 1);
  OpenOptions options;
  options.write_index_checkpoint = false;
  auto repo = Repository::Open(fs, "/r", me, options);
  ASSERT_TRUE(repo.ok());
  MemSourceTree tree;
  tree.AddFile("text", std::vector<std::byte>(3 << 20, std::byte{'t'}));
  tree.AddFile("noise", RandomBytes(4, 900000));
  BackupOptions backup;
  auto b = (*repo)->Backup(tree, backup);
  ASSERT_TRUE(b.ok()) << b.status().ToString();
  ASSERT_TRUE((*repo)->WriteIndexCheckpoint().ok());
  auto index_map = [](Repository& r) {
    std::map<Fingerprint, Location> m;
    for (size_t s = 0; s < ChunkIndex::kShardCount; ++s) {
      r.Internals().index.ForEachInShard(s, [&](const IndexEntry& e) { m[e.fp] = e.location(); });
    }
    return m;
  };
  const auto before = index_map(**repo);
  repo->reset();
  repo = Repository::Open(fs, "/r", me, options);
  ASSERT_TRUE(repo.ok());
  EXPECT_EQ((*repo)->open_report().footers_read, 0u);
  EXPECT_EQ(index_map(**repo), before);
  size_t compressed = 0;
  for (const auto& [fp, loc] : before) compressed += loc.codec == Codec::kZstd ? 1 : 0;
  EXPECT_GT(compressed, 0u);
  MemRestoreSink sink;
  ASSERT_TRUE((*repo)->Restore(b->id, sink, RestoreOptions{}).ok());
  EXPECT_TRUE(testing::DiffMemTrees(tree.nodes(), sink.nodes()).empty());
}

TEST_F(IndexCheckpointTest, CloseWritesACheckpointOnlyWhenItIsStale) {
  OpenWith(true, true, /*stale=*/1000);
  Backup(Files(1, 3));
  ASSERT_TRUE(repo_->Close().ok());
  EXPECT_FALSE(CheckpointExists());  // a few containers, below the threshold
  OpenWith(true, true, /*stale=*/2);
  ASSERT_TRUE(repo_->Close().ok());
  EXPECT_TRUE(CheckpointExists());
  OpenWith(true, true, /*stale=*/2);
  EXPECT_EQ(repo_->open_report().footers_read, 0u);
  EXPECT_EQ(repo_->open_report().checkpoint_containers, repo_->open_report().containers);
}

}  // namespace
}  // namespace dds
