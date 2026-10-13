// A hostile sender (TESTING.md 3.8, G8): every malformed or dishonest message yields ERROR, the
// connection ends, and nothing is committed. GC runs on the receiver throughout. Mutation
// have_miss_reserves must make the skipped-blob COMMIT succeed wrongly, which the test sees.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "dds/common/encoding.h"
#include "dds/common/test_hooks.h"
#include "dds/gc/gc_runner.h"
#include "dds/repl/messages.h"
#include "repl/repl_fixture.h"
#include "support/random_data.h"

namespace dds {
namespace {

using std::chrono::milliseconds;
using testing::ReplRepo;

constexpr milliseconds kWait{10000};

// Runs the receiver on its own thread against a scripted sender, with GC looping on the receiver.
class HostilePeerTest : public ::testing::Test {
 protected:
  // The receiving repository's parameters; a subclass may compress (D107).
  virtual RepoParams DstParams() { return {}; }

  void SetUp() override {
    dst_ = std::make_unique<ReplRepo>(DstParams());
    auto [a, b] = MakeSimulatedLink({});
    sender_ = std::move(a);
    receiver_end_ = std::move(b);
    serve_ = std::thread([this] {
      ReceiverOptions options;
      options.idle_timeout = kWait;
      received_ = ServeReplication(*dst_->repo, *receiver_end_, options);
    });
    gc_ = std::thread([this] {
      GcRunner gc(*dst_->repo);
      GcOptions options;
      options.threshold = 0.9;
      while (!stop_gc_) {
        if (!gc.Run(options).ok()) ++gc_failures_;
      }
    });
  }
  void TearDown() override {
    sender_->Close();
    serve_.join();
    stop_gc_ = true;
    gc_.join();
    // GC fails closed on a repository with a missing reachable blob, which only a wrong commit
    // (the mutation test) can produce; otherwise it must never fail.
    if (!expect_gc_failures_) {
      EXPECT_EQ(gc_failures_.load(), 0);
    }
  }

  void Send(MessageType type, std::span<const std::byte> payload) {
    ASSERT_TRUE(WriteFrame(*sender_, type, payload).ok());
  }
  Frame Next() {
    auto f = ReadFrame(*sender_, kWait);
    EXPECT_TRUE(f.ok()) << f.status().ToString();
    return f.ok() ? *f : Frame{};
  }
  void Hello() {
    const std::string params = ChunkingParamsText(dst_->repo->config());
    Send(MessageType::kHello, EncodeHello({kProtocolVersion, src_.repo->config().repo_id,
                                           ChunkingParamsDigest(params), params}));
    ASSERT_EQ(Next().type, MessageType::kHelloOk);
  }
  // The receiver must answer ERROR(Corruption), end the session, and commit nothing.
  void ExpectRejected() {
    const Frame f = Next();
    ASSERT_EQ(f.type, MessageType::kError);
    auto e = DecodeError(f.payload);
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(e->code, StatusCode::kCorruption) << e->message;
    sender_->Close();
    serve_.join();
    serve_ = std::thread([] {});
    EXPECT_EQ(received_.status().code(), StatusCode::kCorruption);
    EXPECT_TRUE(dst_->repo->ListSnapshots()->empty());
  }

  ReplRepo src_;
  std::unique_ptr<ReplRepo> dst_;
  std::unique_ptr<Connection> sender_;
  std::unique_ptr<Connection> receiver_end_;
  Result<ReceiveReport> received_ = Status::Aborted("not run");
  std::thread serve_;
  std::thread gc_;
  std::atomic<bool> stop_gc_{false};
  std::atomic<int> gc_failures_{0};
  bool expect_gc_failures_ = false;
};

TEST_F(HostilePeerTest, BlobThatDoesNotMatchItsFingerprint) {
  Hello();
  const auto real = testing::RandomBytes(1, 500);
  auto fake = real;
  fake[0] ^= std::byte{1};
  Send(MessageType::kPut,
       EncodePut({1, {{BlobType::kData, FingerprintOf(BlobType::kData, real), fake}}}));
  ExpectRejected();
}

TEST_F(HostilePeerTest, DataBlobLargerThanTheMaximumChunk) {
  Hello();
  const auto big = testing::RandomBytes(2, 65537);  // max_size is 65536
  Send(MessageType::kPut,
       EncodePut({1, {{BlobType::kData, FingerprintOf(BlobType::kData, big), big}}}));
  ExpectRejected();
}

TEST_F(HostilePeerTest, PutWithALengthPastTheEnd) {
  Hello();
  Writer w;
  w.U32(1);
  w.U16(1);
  w.U8(1);
  w.Bytes(FingerprintOf(BlobType::kData, testing::RandomBytes(3, 10)).AsBytes());
  w.U32(1000);  // claims 1000 bytes; 10 follow
  w.Bytes(testing::RandomBytes(3, 10));
  Send(MessageType::kPut, w.buffer());
  ExpectRejected();
}

TEST_F(HostilePeerTest, FrameWithABadCrc) {
  Hello();
  auto frame = EncodeFrame(MessageType::kPutAck, EncodePutAck(1));
  frame.back() ^= std::byte{0xFF};
  ASSERT_TRUE(sender_->Send(frame).ok());
  ExpectRejected();
}

TEST_F(HostilePeerTest, MessageBeforeHello) {
  Send(MessageType::kHave,
       EncodeHave({1, {FingerprintOf(BlobType::kData, testing::RandomBytes(4, 9))}}));
  ExpectRejected();
}

// Queries every blob of a real snapshot, then sends all the missing ones except one data chunk,
// then COMMITs. Returns true if the receiver answered COMMIT_OK.
bool CommitAfterSkippingABlob(ReplRepo& src, Connection& c, const SnapshotId& id) {
  const auto blobs = testing::Reachable(src, id);
  std::vector<Fingerprint> fps;
  for (const auto& [fp, length] : blobs) fps.push_back(fp);
  EXPECT_TRUE(WriteFrame(c, MessageType::kHave, EncodeHave({1, fps})).ok());
  auto reply = ReadFrame(c, kWait);
  EXPECT_TRUE(reply.ok() && reply->type == MessageType::kHaveReply);
  auto present = DecodeHaveReply(reply->payload);
  EXPECT_TRUE(present.ok());
  Put put{2, {}};
  bool skipped = false;
  RepoInternals& in = src.repo->Internals();
  for (size_t i = 0; i < fps.size(); ++i) {
    if (present->present[i]) continue;
    const Location loc = *in.index.Lookup(fps[i]);
    if (!skipped && loc.type == BlobType::kData) {
      skipped = true;  // the blob the COMMIT will reference without ever sending
      continue;
    }
    put.blobs.push_back({loc.type, fps[i], *in.containers.Read(loc)});
  }
  EXPECT_TRUE(skipped);
  EXPECT_TRUE(WriteFrame(c, MessageType::kPut, EncodePut(put)).ok());
  auto ack = ReadFrame(c, kWait);
  EXPECT_TRUE(ack.ok() && ack->type == MessageType::kPutAck);
  EXPECT_TRUE(WriteFrame(c, MessageType::kCommit, *in.snapshots.ReadBytes(id)).ok());
  auto answer = ReadFrame(c, kWait);
  return answer.ok() && answer->type == MessageType::kCommitOk;
}

TEST_F(HostilePeerTest, CommitReferencingASkippedBlob) {
  const MemTree tree = {
      {"f", MemNode{EntryKind::kFile, false, 0644, 0, 0, testing::RandomBytes(5, 40000), {}}}};
  const SnapshotId id = src_.Backup(tree);
  Hello();
  EXPECT_FALSE(CommitAfterSkippingABlob(src_, *sender_, id));
  sender_->Close();
  serve_.join();
  serve_ = std::thread([] {});
  EXPECT_EQ(received_.status().code(), StatusCode::kCorruption);
  EXPECT_TRUE(dst_->repo->ListSnapshots()->empty());
}

TEST_F(HostilePeerTest, CommitReferencingASkippedBlobSucceedsWronglyWhenHaveReserves) {
  if (!test_hooks::kEnabled) GTEST_SKIP() << "built without DDS_ENABLE_TEST_HOOKS";
  test_hooks::ScopedMutation broken("have_miss_reserves");
  expect_gc_failures_ = true;
  const MemTree tree = {
      {"f", MemNode{EntryKind::kFile, false, 0644, 0, 0, testing::RandomBytes(5, 40000), {}}}};
  const SnapshotId id = src_.Backup(tree);
  Hello();
  // The reserved fingerprint counts as present, so the COMMIT is accepted: the bug D55 prevents.
  EXPECT_TRUE(CommitAfterSkippingABlob(src_, *sender_, id));
  EXPECT_FALSE(dst_->Restores(id, tree)) << "the replica committed a snapshot it cannot restore";
}

// A COMMIT whose recipe states the wrong length for a chunk the receiver already holds: the
// receiver must refuse it, or a snapshot that can never be restored would be committed. With
// compression (D107) the chunk is stored as a smaller frame, so the receiver must compare the
// content length its frame header records, not the stored length.
class WrongChunkLengthTest : public HostilePeerTest, public ::testing::WithParamInterface<bool> {
 protected:
  RepoParams DstParams() override {
    RepoParams params;
    if (GetParam()) params.compression = {Codec::kZstd, 3};
    return params;
  }
};

TEST_P(WrongChunkLengthTest, CommitIsRefused) {
  const std::vector<std::byte> content(30000, std::byte{'a'});
  const MemTree honest = {{"f", MemNode{EntryKind::kFile, false, 0644, 0, 0, content, {}}}};
  dst_->Backup(honest);  // the receiver now holds the chunk
  const Fingerprint chunk = FingerprintOf(BlobType::kData, content);
  const auto loc = dst_->repo->Internals().index.Lookup(chunk);
  ASSERT_TRUE(loc.has_value());
  EXPECT_EQ(loc->codec, GetParam() ? Codec::kZstd : Codec::kNone);
  Recipe recipe;
  recipe.refs.push_back({chunk, 29999});  // one byte short
  const auto recipe_bytes = EncodeRecipe(recipe);
  const Fingerprint recipe_fp = FingerprintOf(BlobType::kRecipe, recipe_bytes);
  Tree tree;
  TreeEntry file;
  file.name = "f";
  file.mode = 0644;
  file.size = 29999;
  file.pieces = {recipe_fp};
  tree.entries.push_back(file);
  const auto tree_bytes = *EncodeTree(tree);
  Manifest m;
  m.origin_repo_id = src_.repo->config().repo_id;
  m.root = FingerprintOf(BlobType::kTree, tree_bytes);
  Hello();
  Put put{1, {}};
  put.blobs.push_back({BlobType::kRecipe, recipe_fp, recipe_bytes});
  put.blobs.push_back({BlobType::kTree, m.root, tree_bytes});
  Send(MessageType::kPut, EncodePut(put));
  ASSERT_EQ(Next().type, MessageType::kPutAck);
  Send(MessageType::kCommit, EncodeManifest(m));
  const Frame f = Next();
  ASSERT_EQ(f.type, MessageType::kError);
  auto e = DecodeError(f.payload);
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(e->code, StatusCode::kCorruption);
  EXPECT_NE(e->message.find("length differs from its recipe"), std::string::npos) << e->message;
  EXPECT_EQ(dst_->repo->ListSnapshots()->size(), 1u);  // only the receiver's own backup
}

INSTANTIATE_TEST_SUITE_P(PlainAndCompressed, WrongChunkLengthTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& param_info) {
                           return param_info.param ? std::string("Zstd") : std::string("Plain");
                         });

}  // namespace
}  // namespace dds
