// Subtree pruning in replication (protocol version 2, D108): the sender asks about
// every tree in one pass and skips the subtrees the receiver holds whole; the receiver answers by
// checking the whole subtree itself, stamping what it finds, and its COMMIT walk skips what it
// found whole.

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "dds/common/test_hooks.h"
#include "dds/gc/gc_runner.h"
#include "dds/repl/messages.h"
#include "repl/repl_fixture.h"
#include "support/random_data.h"

namespace dds {
namespace {

using std::chrono::milliseconds;
using testing::RandomBytes;
using testing::ReplicateOverLink;
using testing::ReplRepo;

// Ten directories of four files each; `changed` (if not empty) gets new content in one file.
MemTree TenDirs(const std::string& changed = "") {
  MemSourceTree t;
  for (int d = 0; d < 10; ++d) {
    const std::string dir = "d" + std::to_string(d);
    t.AddDir(dir, 0755, 100);
    for (int f = 0; f < 4; ++f) {
      t.AddFile(dir + "/f" + std::to_string(f),
                RandomBytes(static_cast<uint64_t>(d * 10 + f), 3000 + 997 * static_cast<size_t>(f)),
                0644, 100);
    }
  }
  MemTree nodes = t.nodes();
  if (!changed.empty()) nodes[changed + "/f0"].content = RandomBytes(999, 5000);
  return nodes;
}

// A receiver session driven frame by frame.
class ScriptedSession {
 public:
  explicit ScriptedSession(Repository& dst) {
    auto [a, b] = MakeSimulatedLink({});
    sender_ = std::move(a);
    receiver_end_ = std::move(b);
    serve_ = std::thread([this, &dst] {
      ReceiverOptions options;
      options.idle_timeout = milliseconds(10000);
      received_ = ServeReplication(dst, *receiver_end_, options);
    });
  }
  ~ScriptedSession() { (void)End(); }
  // Closes the connection and returns what the receiver returned.
  Result<ReceiveReport> End() {
    sender_->Close();
    if (serve_.joinable()) serve_.join();
    return received_;
  }
  void Send(MessageType type, std::span<const std::byte> payload) {
    ASSERT_TRUE(WriteFrame(*sender_, type, payload).ok());
  }
  Frame Next() {
    // A bound against hanging, not a speed claim: with GCC's AddressSanitizer, whose run time
    // gives every call a fake stack frame, the 50,000-tree chain below takes about 24 s.
    auto f = ReadFrame(*sender_, milliseconds(120000));
    EXPECT_TRUE(f.ok()) << f.status().ToString();
    return f.ok() ? *f : Frame{};
  }
  void Hello(Repository& src, uint16_t version) {
    const std::string params = ChunkingParamsText(src.config());
    Send(MessageType::kHello,
         EncodeHello({version, src.config().repo_id, ChunkingParamsDigest(params), params}));
    const Frame f = Next();
    ASSERT_EQ(f.type, MessageType::kHelloOk);
    EXPECT_EQ(DecodeHelloOk(f.payload)->version, version);
  }
  void Put(const std::vector<PutBlob>& blobs) {
    Send(MessageType::kPut, EncodePut({1, blobs}));
    ASSERT_EQ(Next().type, MessageType::kPutAck);
  }
  // Asks whether `trees` are whole and returns the answers.
  std::vector<bool> AskTrees(const std::vector<Fingerprint>& trees) {
    Send(MessageType::kHaveTrees, EncodeHave({1, trees}));
    const Frame f = Next();
    EXPECT_EQ(f.type, MessageType::kHaveTreesReply);
    auto reply = DecodeHaveReply(f.payload);
    return reply.ok() ? reply->present : std::vector<bool>{};
  }

 private:
  std::unique_ptr<Connection> sender_;
  std::unique_ptr<Connection> receiver_end_;
  Result<ReceiveReport> received_ = Status::Aborted("not run");
  std::thread serve_;
};

void ExpectReplicated(const testing::ReplResult& r) {
  ASSERT_TRUE(r.sent.ok()) << r.sent.status().ToString() << " / " << r.received.status().ToString();
  ASSERT_TRUE(r.received.ok()) << r.received.status().ToString();
}

TEST(ReplicationPruningTest, UnchangedSubtreesAreNotQueried) {
  ReplRepo src;
  const SnapshotId first = src.Backup(TenDirs());
  const MemTree second_tree = TenDirs("d3");
  const SnapshotId second = src.Backup(second_tree);
  ReplRepo pruned;
  ReplRepo unpruned;
  for (ReplRepo* dst : {&pruned, &unpruned}) ExpectReplicated(ReplicateOverLink(src, first, *dst));
  SenderOptions off;
  off.prune_subtrees = false;
  const auto with = ReplicateOverLink(src, second, pruned);
  const auto without = ReplicateOverLink(src, second, unpruned, {}, off);
  ExpectReplicated(with);
  ExpectReplicated(without);
  // The root and d3 changed; the nine other directories are whole on the receiver.
  EXPECT_EQ(with.sent->trees_checked, 11u);
  EXPECT_EQ(with.sent->subtrees_pruned, 9u);
  EXPECT_EQ(with.received->subtrees_whole, 9u);
  EXPECT_EQ(without.sent->trees_checked, 0u);
  // Queried: the root and d3 trees and d3's four files' recipes and chunks, against every blob.
  EXPECT_LT(with.sent->fingerprints_queried * 5, without.sent->fingerprints_queried);
  EXPECT_EQ(with.sent->payload_bytes_sent, without.sent->payload_bytes_sent);
  for (ReplRepo* dst : {&pruned, &unpruned}) {
    EXPECT_TRUE(dst->Restores(second, second_tree));
    EXPECT_TRUE(dst->Verifies());
  }
}

// Trees are asked about in batches as they are found, within the window: with batches of 2 and a
// window of 1 the eleven trees take six HAVE_TREES, each waiting for the last one's answer.
TEST(ReplicationPruningTest, TreesAreAskedInBatchesAsTheyAreFound) {
  ReplRepo src;
  const SnapshotId first = src.Backup(TenDirs());
  const MemTree second_tree = TenDirs("d3");
  const SnapshotId second = src.Backup(second_tree);
  ReplRepo dst;
  ExpectReplicated(ReplicateOverLink(src, first, dst));
  SenderOptions small;
  small.have_batch = 2;
  small.window = 1;
  const auto r = ReplicateOverLink(src, second, dst, {}, small);
  ExpectReplicated(r);
  EXPECT_EQ(r.sent->trees_checked, 11u);
  EXPECT_EQ(r.sent->subtrees_pruned, 9u);
  EXPECT_EQ(r.sent->wire_bytes_sent.at(MessageType::kHaveTrees),
            6 * (kFrameOverhead + 1 + 6) + 11 * sizeof(Fingerprint::bytes));
  EXPECT_TRUE(dst.Restores(second, second_tree));
}

TEST(ReplicationPruningTest, AWholeSnapshotSendsNoQueriesAtAll) {
  ReplRepo src;
  const MemTree tree = TenDirs();
  const SnapshotId first = src.Backup(tree);
  ReplRepo dst;
  ExpectReplicated(ReplicateOverLink(src, first, dst));
  const SnapshotId again = src.Backup(tree);  // same root, another manifest
  const auto r = ReplicateOverLink(src, again, dst);
  ExpectReplicated(r);
  EXPECT_EQ(r.sent->trees_checked, 11u);   // every tree is asked about, in one pass
  EXPECT_EQ(r.sent->subtrees_pruned, 1u);  // the root, so nothing below it is walked
  EXPECT_EQ(r.sent->fingerprints_queried, 0u);
  EXPECT_TRUE(dst.Restores(again, tree));
}

// Two directories with the same content are one tree: asked about once, and both restore.
TEST(ReplicationPruningTest, ATreeInTwoPlacesIsAskedAboutOnce) {
  MemSourceTree t;
  for (const std::string dir : {"a", "b"}) {
    t.AddDir(dir, 0755, 100);
    t.AddFile(dir + "/f", RandomBytes(7, 20000), 0644, 100);
  }
  ReplRepo src;
  const SnapshotId id = src.Backup(t.nodes());
  ReplRepo dst;
  const auto r = ReplicateOverLink(src, id, dst);
  ExpectReplicated(r);
  EXPECT_EQ(r.sent->trees_checked, 2u);  // the root and the one tree both directories name
  EXPECT_TRUE(dst.Restores(id, t.nodes()));
}

// A tree blob alone, without what lies below it, is not whole: the receiver checks the subtree,
// not just the tree, which is why pruning needs no invariant from GC (D108).
TEST(ReplicationPruningTest, ATreeWithoutItsDescendantsIsNotWhole) {
  ReplRepo src;
  const MemTree tree = TenDirs();
  const SnapshotId id = src.Backup(tree);
  RepoInternals& in = src.repo->Internals();
  auto root = DecodeTree(*in.containers.Read(*in.index.Lookup(in.snapshots.Read(id)->root)));
  ASSERT_TRUE(root.ok());
  const Fingerprint d3 = root->entries[3].subtree;
  ReplRepo dst;
  {
    ScriptedSession s(*dst.repo);  // gives the receiver d3's tree blob and nothing below it
    s.Hello(*src.repo, kProtocolVersion);
    s.Put({{BlobType::kTree, d3, *in.containers.Read(*in.index.Lookup(d3))}});
  }
  const auto r = ReplicateOverLink(src, id, dst);
  ExpectReplicated(r);
  EXPECT_EQ(r.sent->trees_checked, 11u);  // the root and its ten directories
  EXPECT_EQ(r.sent->subtrees_pruned, 0u);
  EXPECT_EQ(r.received->trees_remembered, 1u);  // d3, held here; never the absent trees asked
  EXPECT_TRUE(dst.Restores(id, tree));
  EXPECT_TRUE(dst.Verifies());
}

TEST(ReplicationPruningTest, HaveTreesInAVersion1SessionIsCorruption) {
  ReplRepo src;
  ReplRepo dst;
  ScriptedSession s(*dst.repo);
  s.Hello(*src.repo, 1);
  s.Send(MessageType::kHaveTrees, EncodeHave({1, {Fingerprint{}}}));
  const Frame f = s.Next();
  ASSERT_EQ(f.type, MessageType::kError);
  EXPECT_EQ(DecodeError(f.payload)->code, StatusCode::kCorruption);
}

// The receiver holds the snapshot's blobs only as garbage: its own snapshot of the same tree was
// removed. HAVE_TREES finds the root whole, a whole GC runs, then COMMIT arrives and skips the
// subtree it found whole. The check's stamps must have kept every blob (6.2 item 3).
struct CommitAfterGc {
  bool committed = false;
  bool restores = false;
};

CommitAfterGc CommitWholeSubtreeAfterGc() {
  ReplRepo src;
  const MemTree tree = TenDirs();
  const SnapshotId id = src.Backup(tree);
  const Fingerprint root = src.repo->Internals().snapshots.Read(id)->root;
  ReplRepo dst;
  const SnapshotId own = dst.Backup(tree);  // the same trees, so the same root
  auto gc = [&dst] {
    GcRunner runner(*dst.repo);
    GcOptions options;
    options.threshold = 1.0;
    EXPECT_TRUE(runner.Run(options).ok());
  };
  gc();  // moves the epoch past the stamps of dst's own backup
  EXPECT_TRUE(dst.repo->RemoveSnapshot(own).ok());
  CommitAfterGc result;
  {
    ScriptedSession s(*dst.repo);
    s.Hello(*src.repo, kProtocolVersion);
    EXPECT_EQ(s.AskTrees({root}), std::vector<bool>{true});
    gc();
    s.Send(MessageType::kCommit, *src.repo->Internals().snapshots.ReadBytes(id));
    result.committed = s.Next().type == MessageType::kCommitOk;
  }
  result.restores = dst.Restores(id, tree);
  return result;
}

TEST(ReplicationPruningTest, SubtreesFoundWholeSurviveGcUntilCommit) {
  const CommitAfterGc r = CommitWholeSubtreeAfterGc();
  EXPECT_TRUE(r.committed);
  EXPECT_TRUE(r.restores);
}

TEST(ReplicationPruningTest, WithoutTheCheckStampingGcLosesThem) {
  if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  test_hooks::ScopedMutation broken("subtree_check_without_stamp");
  const CommitAfterGc r = CommitWholeSubtreeAfterGc();
  // The COMMIT is accepted for a subtree GC has since removed: the bug the stamps prevent.
  EXPECT_TRUE(r.committed);
  EXPECT_FALSE(r.restores) << "the replica committed a snapshot it cannot restore";
}

// A one-file snapshot whose recipe gives `chunk` the length `ref_length`.
struct OneFile {
  PutBlob chunk, recipe, tree;
  Manifest manifest;
};

OneFile OneFileSnapshot(Repository& src, PutBlob chunk, uint32_t ref_length) {
  OneFile f;
  f.chunk = std::move(chunk);
  Recipe recipe;
  recipe.refs.push_back({f.chunk.fp, ref_length});
  auto recipe_bytes = EncodeRecipe(recipe);
  f.recipe = {BlobType::kRecipe, FingerprintOf(BlobType::kRecipe, recipe_bytes), recipe_bytes};
  Tree tree;
  TreeEntry file;
  file.name = "f";
  file.mode = 0644;
  file.size = ref_length;
  file.pieces = {f.recipe.fp};
  tree.entries.push_back(file);
  auto tree_bytes = *EncodeTree(tree);
  f.tree = {BlobType::kTree, FingerprintOf(BlobType::kTree, tree_bytes), tree_bytes};
  f.manifest.origin_repo_id = src.config().repo_id;
  f.manifest.root = f.tree.fp;
  return f;
}

// The recipe and tree arrive in one session, sealed when it ends; in the next, the chunk arrives
// (not sealed: no index entry yet), HAVE_TREES asks about the root, and COMMIT follows. A chunk
// that cannot be checked yet must make the subtree not whole, so COMMIT, which seals first, checks
// it; found by review of D108's first version, which skipped such chunks and answered whole.
struct AskThenCommit {
  bool whole = true;
  Frame commit_reply;
};

AskThenCommit SendChunkAskAndCommit(Repository& src, Repository& dst, const OneFile& f) {
  {
    ScriptedSession s(dst);
    s.Hello(src, kProtocolVersion);
    s.Put({f.recipe, f.tree});
  }
  ScriptedSession s(dst);
  s.Hello(src, kProtocolVersion);
  s.Put({f.chunk});
  AskThenCommit r;
  const std::vector<bool> answer = s.AskTrees({f.tree.fp});
  r.whole = answer.empty() || answer[0];
  s.Send(MessageType::kCommit, EncodeManifest(f.manifest));
  r.commit_reply = s.Next();
  return r;
}

TEST(ReplicationPruningTest, AnUnsealedChunkIsNotFoundWhole) {
  ReplRepo src;
  ReplRepo dst;
  const auto content = RandomBytes(11, 30000);
  const OneFile f = OneFileSnapshot(
      *src.repo, {BlobType::kData, FingerprintOf(BlobType::kData, content), content}, 30000);
  const AskThenCommit r = SendChunkAskAndCommit(*src.repo, *dst.repo, f);
  EXPECT_FALSE(r.whole);
  ASSERT_EQ(r.commit_reply.type, MessageType::kCommitOk);
  auto id = DecodeCommitOk(r.commit_reply.payload);
  ASSERT_TRUE(id.ok());
  const MemTree expected = {{"f", MemNode{EntryKind::kFile, false, 0644, 0, 0, content, {}}}};
  EXPECT_TRUE(dst.Restores(*id, expected));
}

void ExpectRefused(const AskThenCommit& r, ReplRepo& dst) {
  EXPECT_FALSE(r.whole);
  ASSERT_EQ(r.commit_reply.type, MessageType::kError);
  auto e = DecodeError(r.commit_reply.payload);
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(e->code, StatusCode::kCorruption) << e->message;
  EXPECT_TRUE(dst.repo->ListSnapshots()->empty());
}

TEST(ReplicationPruningTest, AWrongLengthBehindAnUnsealedChunkIsRefused) {
  ReplRepo src;
  ReplRepo dst;
  const auto content = RandomBytes(12, 30000);
  const OneFile f = OneFileSnapshot(
      *src.repo, {BlobType::kData, FingerprintOf(BlobType::kData, content), content}, 29999);
  ExpectRefused(SendChunkAskAndCommit(*src.repo, *dst.repo, f), dst);
}

TEST(ReplicationPruningTest, ATreeReferencedAsAChunkBehindAnUnsealedBlobIsRefused) {
  ReplRepo src;
  ReplRepo dst;
  const auto empty_tree = *EncodeTree(Tree{});
  const OneFile f = OneFileSnapshot(
      *src.repo, {BlobType::kTree, FingerprintOf(BlobType::kTree, empty_tree), empty_tree},
      static_cast<uint32_t>(empty_tree.size()));
  ExpectRefused(SendChunkAskAndCommit(*src.repo, *dst.repo, f), dst);
}

// A peer may ask HAVE_TREES about fingerprints it made up, as many as it likes: each is answered
// not whole and leaves nothing behind, or a peer could grow the receiver's memory without bound.
TEST(ReplicationPruningTest, MadeUpFingerprintsAreNotRemembered) {
  ReplRepo src;
  ReplRepo dst;
  const auto content = RandomBytes(13, 5000);
  const OneFile f = OneFileSnapshot(
      *src.repo, {BlobType::kData, FingerprintOf(BlobType::kData, content), content}, 5000);
  ScriptedSession s(*dst.repo);
  s.Hello(*src.repo, kProtocolVersion);
  for (uint64_t batch = 0; batch < 3; ++batch) {
    std::vector<Fingerprint> made_up;
    for (uint64_t i = 0; i < kMaxHaveCount; ++i) {
      made_up.push_back(FingerprintOf(BlobType::kTree, RandomBytes(batch * kMaxHaveCount + i, 8)));
    }
    EXPECT_EQ(s.AskTrees(made_up), std::vector<bool>(kMaxHaveCount, false));
  }
  // A COMMIT, so the session ends normally and returns its report.
  s.Put({f.chunk, f.recipe, f.tree});
  s.Send(MessageType::kCommit, EncodeManifest(f.manifest));
  ASSERT_EQ(s.Next().type, MessageType::kCommitOk);
  auto report = s.End();
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  EXPECT_EQ(report->trees_checked, 3 * kMaxHaveCount);
  EXPECT_EQ(report->trees_remembered, 0u);
}

// A chain of 50,000 nested trees held by the receiver: the check walks it with an explicit stack,
// so a peer cannot make it recurse until the stack overflows.
TEST(ReplicationPruningTest, AVeryDeepChainOfTreesIsCheckedWithoutRecursion) {
  ReplRepo src;
  ReplRepo dst;
  std::vector<PutBlob> chain;
  for (int depth = 0; depth < 50000; ++depth) {
    Tree t;
    if (!chain.empty()) {
      TreeEntry e;
      e.name = "d";
      e.kind = EntryKind::kDirectory;
      e.mode = 0755;
      e.subtree = chain.back().fp;
      t.entries.push_back(e);
    }
    auto bytes = *EncodeTree(t);
    const Fingerprint fp = FingerprintOf(BlobType::kTree, bytes);
    chain.push_back({BlobType::kTree, fp, std::move(bytes)});
  }
  {
    ScriptedSession s(*dst.repo);
    s.Hello(*src.repo, kProtocolVersion);
    s.Put(chain);
  }
  ScriptedSession s(*dst.repo);
  s.Hello(*src.repo, kProtocolVersion);
  EXPECT_EQ(s.AskTrees({chain.back().fp}), std::vector<bool>{true});
}

}  // namespace
}  // namespace dds
