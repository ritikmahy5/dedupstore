// Replication (TESTING.md 3.8; PROTOCOL.md; G8, G9) over SimulatedLink: correctness, exact
// transfer accounting, resumption after disconnects with GC running on the receiver, and
// incompatible parameters.

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>

#include "dds/gc/gc_runner.h"
#include "repl/repl_fixture.h"
#include "support/test_seed.h"
#include "support/tree_generator.h"

namespace dds {
namespace {

using testing::ReplicateOverLink;
using testing::ReplRepo;
using testing::TreeGenerator;
using testing::TreeGenOptions;

TreeGenOptions SmallTrees() {
  TreeGenOptions o;
  o.files = 15;
  o.max_file_size = 128 << 10;
  return o;
}

TEST(ReplicationTest, ReplicaRestoresAndOnlyMissingBlobsCross) {
  ReplRepo src;
  ReplRepo dst;
  TreeGenerator gen(testing::TestSeed(), SmallTrees());
  MemTree tree = gen.NewTree();
  const SnapshotId first = src.Backup(tree);
  auto r1 = ReplicateOverLink(src, first, dst);
  ASSERT_TRUE(r1.sent.ok()) << r1.sent.status().ToString() << " / "
                            << r1.received.status().ToString();
  ASSERT_TRUE(r1.received.ok()) << r1.received.status().ToString();
  EXPECT_TRUE(dst.Restores(first, tree));
  EXPECT_TRUE(dst.Verifies());

  // G9: a similar second snapshot sends exactly the blobs the replica lacks, computed from both
  // indexes before the run.
  const MemTree first_tree = tree;
  gen.Mutate(tree);
  const SnapshotId second = src.Backup(tree);
  uint64_t lacked = 0;
  for (const auto& [fp, length] : testing::Reachable(src, second)) {
    if (!dst.repo->Internals().index.Lookup(fp).has_value()) lacked += length;
  }
  auto r2 = ReplicateOverLink(src, second, dst);
  ASSERT_TRUE(r2.sent.ok()) << r2.sent.status().ToString();
  EXPECT_EQ(r2.sent->payload_bytes_sent, lacked);
  EXPECT_GT(r2.sent->fingerprints_queried, r2.sent->blobs_sent);
  EXPECT_TRUE(dst.Restores(second, tree));
  EXPECT_TRUE(dst.Restores(first, first_tree));
  EXPECT_EQ(dst.repo->ListSnapshots()->size(), 2u);
  EXPECT_TRUE(dst.Verifies());
}

// The number of index entries of `repo` stored with `codec`.
size_t EntriesWith(ReplRepo& repo, Codec codec) {
  size_t n = 0;
  for (size_t s = 0; s < ChunkIndex::kShardCount; ++s) {
    repo.repo->Internals().index.ForEachInShard(
        s, [&](const IndexEntry& e) { n += e.codec == codec ? 1 : 0; });
  }
  return n;
}

// D107: compression is each repository's own choice, and the protocol carries content. A compressed
// source sends content that an uncompressed replica stores as it is; a compressed replica receiving
// it back compresses it again. Every copy restores and verifies.
TEST(ReplicationTest, CompressionIsEachRepositorysOwnChoice) {
  RepoParams zstd;
  zstd.compression = {Codec::kZstd, 3};
  ReplRepo compressed(zstd);
  ReplRepo plain;
  ReplRepo again(zstd);
  TreeGenOptions options = SmallTrees();
  options.compressible = true;
  TreeGenerator gen(testing::TestSeed(), options);
  const MemTree tree = gen.NewTree();
  const SnapshotId id = compressed.Backup(tree);
  ASSERT_GT(EntriesWith(compressed, Codec::kZstd), 0u);
  for (auto [from, to] : {std::pair{&compressed, &plain}, std::pair{&plain, &again}}) {
    auto r = ReplicateOverLink(*from, id, *to);
    ASSERT_TRUE(r.sent.ok()) << r.sent.status().ToString() << " / "
                             << r.received.status().ToString();
    ASSERT_TRUE(r.received.ok()) << r.received.status().ToString();
    EXPECT_TRUE(to->Restores(id, tree));
    EXPECT_TRUE(to->Verifies());
  }
  EXPECT_EQ(EntriesWith(plain, Codec::kZstd), 0u);
  EXPECT_EQ(EntriesWith(again, Codec::kZstd), EntriesWith(compressed, Codec::kZstd));
}

TEST(ReplicationTest, ReplicatingTwiceIsIdempotent) {
  ReplRepo src;
  ReplRepo dst;
  const MemTree tree = TreeGenerator(testing::TestSeed(), SmallTrees()).NewTree();
  const SnapshotId id = src.Backup(tree);
  ASSERT_TRUE(ReplicateOverLink(src, id, dst).sent.ok());
  auto again = ReplicateOverLink(src, id, dst);
  ASSERT_TRUE(again.sent.ok()) << again.sent.status().ToString();
  EXPECT_EQ(again.sent->blobs_sent, 0u);
  EXPECT_EQ(dst.repo->ListSnapshots()->size(), 1u);
}

TEST(ReplicationTest, SmallWindowsAndBatchesStillDeliverEverything) {
  ReplRepo src;
  ReplRepo dst;
  const MemTree tree = TreeGenerator(testing::TestSeed() + 7, SmallTrees()).NewTree();
  const SnapshotId id = src.Backup(tree);
  SenderOptions tight;
  tight.window = 1;
  tight.have_batch = 3;
  tight.put_payload_limit = 1000;  // most blobs go alone in their PUT
  auto r = ReplicateOverLink(src, id, dst, {}, tight);
  ASSERT_TRUE(r.sent.ok()) << r.sent.status().ToString();
  EXPECT_GT(r.sent->have_batches, 10u);
  EXPECT_TRUE(dst.Restores(id, tree));
}

TEST(ReplicationTest, IncompatibleChunkingIsRefused) {
  ReplRepo src;
  RepoParams fixed;
  fixed.chunker.kind = ChunkerKind::kFixed;
  fixed.chunker.min_size = fixed.chunker.avg_size = fixed.chunker.max_size = 4096;
  fixed.chunker.normalization = 0;
  ReplRepo dst(fixed);
  const SnapshotId id = src.Backup(TreeGenerator(1, SmallTrees()).NewTree());
  auto r = ReplicateOverLink(src, id, dst);
  EXPECT_EQ(r.sent.status().code(), StatusCode::kIncompatible) << r.sent.status().ToString();
  EXPECT_NE(r.sent.status().message().find("chunker=fixed"), std::string::npos);
  EXPECT_TRUE(dst.repo->ListSnapshots()->empty());
}

// Runs GC on `dst` in a loop until `stop`, as TESTING.md 3.8 asks during disconnects.
class GcLoop {
 public:
  explicit GcLoop(Repository& repo)
      : thread_([this, &repo] {
          GcRunner gc(repo);
          GcOptions options;
          options.threshold = 0.9;
          while (!stop_) {
            auto r = gc.Run(options);
            EXPECT_TRUE(r.ok()) << r.status().ToString();
          }
        }) {}
  ~GcLoop() {
    stop_ = true;
    thread_.join();
  }

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

TEST(ReplicationTest, DisconnectsAtRandomOffsetsResumeAndCommitOnce) {
  // 50 seeded runs: each breaks the link at a random byte offset, then reruns the same transfer.
  ReplRepo src;
  const MemTree tree = TreeGenerator(testing::TestSeed(), SmallTrees()).NewTree();
  const SnapshotId id = src.Backup(tree);
  uint64_t total = 0;
  {
    ReplRepo probe;
    auto full = ReplicateOverLink(src, id, probe);
    ASSERT_TRUE(full.sent.ok());
    for (const auto& [type, bytes] : full.sent->wire_bytes_sent) total += bytes;
    total += full.sent->wire_bytes_received;
  }
  std::mt19937_64 rng(testing::TestSeed());
  for (int run = 0; run < 50; ++run) {
    SCOPED_TRACE(::testing::Message() << "run " << run);
    ReplRepo dst;
    GcLoop gc(*dst.repo);
    LinkOptions broken;
    broken.disconnect_after_bytes = rng() % total;
    auto interrupted = ReplicateOverLink(src, id, dst, broken);
    // The transfer's bytes are the same every time, and COMMIT_OK is its last message, so a cut
    // anywhere before the end leaves the sender without it.
    EXPECT_FALSE(interrupted.sent.ok())
        << "cut at " << *broken.disconnect_after_bytes << " of " << total;
    auto resumed = ReplicateOverLink(src, id, dst);
    ASSERT_TRUE(resumed.sent.ok()) << resumed.sent.status().ToString();
    auto listed = dst.repo->ListSnapshots();
    ASSERT_TRUE(listed.ok());
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->at(0).id, id);
    EXPECT_TRUE(dst.Restores(id, tree));
    EXPECT_TRUE(dst.Verifies());
  }
}

}  // namespace
}  // namespace dds
