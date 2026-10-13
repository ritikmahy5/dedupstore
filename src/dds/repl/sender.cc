#include "dds/repl/sender.h"

#include <algorithm>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "dds/common/ordered_mutex.h"
#include "dds/common/strings.h"
#include "dds/repl/messages.h"
#include "dds/repl/send_window.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/tree.h"

namespace dds {

namespace {

class Sender {
 public:
  Sender(RepoInternals& repo, Connection& c, const SenderOptions& options, SendReport& report)
      : repo_(repo), c_(c), options_(options), report_(report), window_(options.window) {}

  Status Hello() {
    const std::string params = ChunkingParamsText(repo_.config);
    // Without pruning nothing needs version 2, so the sender asks for 1 and can still reach a
    // receiver built before version 2 existed (D108).
    const uint16_t version = options_.prune_subtrees ? kProtocolVersion : kOldestProtocolVersion;
    DDS_TRY(Write(MessageType::kHello, EncodeHello({version, repo_.config.repo_id,
                                                    ChunkingParamsDigest(params), params})));
    auto frame = ReadFrame(c_, options_.reply_timeout);
    DDS_TRY(frame.status());
    report_.wire_bytes_received += frame->payload.size() + 1 + kFrameOverhead;
    if (frame->type == MessageType::kError) return ErrorStatus(frame->payload);
    if (frame->type != MessageType::kHelloOk) return Status::Corruption("expected HELLO_OK");
    auto ok = DecodeHelloOk(frame->payload);
    DDS_TRY(ok.status());
    if (ok->version < kOldestProtocolVersion || ok->version > version) {
      return Status::Incompatible(StrCat("receiver protocol version ", ok->version));
    }
    version_ = ok->version;
    return Status::Ok();
  }

  // The reader thread's loop.
  void ReadReplies() {
    while (true) {
      auto frame = ReadFrame(c_, options_.reply_timeout);
      if (!frame.ok()) return window_.Fail(frame.status());
      report_.wire_bytes_received += frame->payload.size() + 1 + kFrameOverhead;
      const Status handled = OnReply(*frame);
      if (!handled.ok()) return window_.Fail(handled);
      if (frame->type == MessageType::kCommitOk) return;
    }
  }

  // The writer's work: walk, query, send what is missing, commit.
  Result<SnapshotId> Send(const SnapshotId& id) {
    auto manifest_bytes = repo_.snapshots.ReadBytes(id);
    DDS_TRY(manifest_bytes.status());
    auto manifest = DecodeManifest(*manifest_bytes);
    DDS_TRY(manifest.status());
    if (version_ >= 2 && options_.prune_subtrees) {
      std::vector<Fingerprint> to_walk;
      DDS_TRY(FindTreesToWalk(manifest->root, to_walk));
      for (const Fingerprint& fp : to_walk) DDS_TRY(WalkTree(fp, nullptr));
    } else {
      DDS_TRY(Walk(manifest->root));
    }
    DDS_TRY(FlushHave());
    while (!window_.Quiet()) {
      if (!window_.WaitForQuietOrWork()) return window_.error();
      DDS_TRY(SendMissing());
    }
    DDS_TRY(Write(MessageType::kCommit, *manifest_bytes));
    return window_.WaitForCommit();
  }

  Window& window() { return window_; }

 private:
  Status OnReply(const Frame& f) {
    switch (f.type) {
      case MessageType::kHaveReply: {
        auto reply = DecodeHaveReply(f.payload);
        DDS_TRY(reply.status());
        return window_.HaveAnswered(*reply);
      }
      case MessageType::kHaveTreesReply: {
        auto reply = DecodeHaveReply(f.payload);
        DDS_TRY(reply.status());
        return window_.TreesAnswered(*reply);
      }
      case MessageType::kPutAck: {
        auto batch = DecodePutAck(f.payload);
        DDS_TRY(batch.status());
        return window_.PutAnswered(*batch);
      }
      case MessageType::kCommitOk: {
        auto id = DecodeCommitOk(f.payload);
        DDS_TRY(id.status());
        window_.CommitOk(*id);
        return Status::Ok();
      }
      case MessageType::kError:
        return ErrorStatus(f.payload);
      default:
        return Status::Corruption(
            StrCat("unexpected ", MessageTypeName(f.type), " from a receiver"));
    }
  }

  static Status ErrorStatus(std::span<const std::byte> payload) {
    auto e = DecodeError(payload);
    DDS_TRY(e.status());
    return StatusFromError(*e);
  }

  Status Write(MessageType type, std::span<const std::byte> payload) {
    auto n = WriteFrame(c_, type, payload);
    if (!n.ok()) {
      // A broken send usually has a better explanation from the reader (an ERROR, a timeout).
      const Status reason = window_.error();
      return reason.ok() ? n.status() : reason;
    }
    report_.wire_bytes_sent[type] += *n;
    return Status::Ok();
  }

  // Streaming depth-first walk: a tree, then its files' recipe pieces and their chunks, then its
  // subdirectories. Only the stack of pending subtrees and the bounded queried set stay in memory.
  Status Walk(const Fingerprint& root) {
    std::vector<Fingerprint> trees = {root};
    while (!trees.empty()) {
      const Fingerprint fp = trees.back();
      trees.pop_back();
      DDS_TRY(WalkTree(fp, &trees));
    }
    return Status::Ok();
  }

  // Queries tree `fp`, then its files' recipe pieces and their chunks, and appends its
  // subdirectories to `subdirs` if given. A tree already queried is skipped, with what lies below.
  Status WalkTree(const Fingerprint& fp, std::vector<Fingerprint>* subdirs) {
    if (!Query(fp)) return Status::Ok();
    auto bytes = ReadBlob(fp, BlobType::kTree);
    DDS_TRY(bytes.status());
    auto tree = DecodeTree(*bytes);
    DDS_TRY(tree.status());
    for (const TreeEntry& e : tree->entries) {
      if (e.kind == EntryKind::kDirectory && subdirs != nullptr) subdirs->push_back(e.subtree);
      if (e.kind != EntryKind::kFile) continue;
      for (const Fingerprint& piece : e.pieces) {
        if (!Query(piece)) continue;
        auto piece_bytes = ReadBlob(piece, BlobType::kRecipe);
        DDS_TRY(piece_bytes.status());
        auto recipe = DecodeRecipe(*piece_bytes, repo_.config.chunker.max_size);
        DDS_TRY(recipe.status());
        for (const ChunkRef& ref : recipe->refs) Query(ref.fp);
      }
    }
    return MaybeSendHave();
  }

  // Protocol version 2 (PROTOCOL.md 4.1 step 2, D108): asks about every distinct tree of the
  // snapshot, then collects in `to_walk`, top-down, the trees the receiver does not hold whole; a
  // whole subtree is never descended into. 32 bytes per directory and no round trip per level.
  Status FindTreesToWalk(const Fingerprint& root, std::vector<Fingerprint>& to_walk) {
    std::vector<Fingerprint> trees = {root};    // every distinct tree, parents before children
    std::vector<std::vector<size_t>> children;  // children[i]: tree i's subdirectories, by index
    std::unordered_map<Fingerprint, size_t, FingerprintHash> index = {{root, 0}};
    std::vector<uint32_t> batches;  // the HAVE_TREES sent, in order, for trees[0, asked)
    size_t asked = 0;
    for (size_t i = 0; i < trees.size(); ++i) {
      auto bytes = ReadBlob(trees[i], BlobType::kTree);
      DDS_TRY(bytes.status());
      auto tree = DecodeTree(*bytes);
      DDS_TRY(tree.status());
      std::vector<size_t> mine;
      for (const TreeEntry& e : tree->entries) {
        if (e.kind != EntryKind::kDirectory) continue;
        // The same directory content in two places is one tree, asked about once.
        const auto [at, added] = index.emplace(e.subtree, trees.size());
        if (added) trees.push_back(e.subtree);
        mine.push_back(at->second);
      }
      children.push_back(std::move(mine));
      // Trees are only appended, so those found so far are final: a full batch goes now. The
      // receiver then never waits longer than one batch's reading, whatever the snapshot's size
      // (its idle timeout, PROTOCOL.md 6), and the round trips overlap the reading.
      while (trees.size() - asked >= options_.have_batch) DDS_TRY(AskTrees(trees, asked, batches));
    }
    while (asked < trees.size()) DDS_TRY(AskTrees(trees, asked, batches));
    auto whole = TreeAnswers(batches);
    DDS_TRY(whole.status());
    std::vector<bool> visited(trees.size(), false);
    std::vector<size_t> stack = {0};
    visited[0] = true;
    while (!stack.empty()) {
      const size_t i = stack.back();
      stack.pop_back();
      if ((*whole)[i]) {
        ++report_.subtrees_pruned;
        continue;
      }
      to_walk.push_back(trees[i]);
      for (const size_t child : children[i]) {
        if (!visited[child]) {
          visited[child] = true;
          stack.push_back(child);
        }
      }
    }
    return Status::Ok();
  }

  // Sends the next HAVE_TREES batch, trees[asked, asked + have_batch), within the window.
  Status AskTrees(const std::vector<Fingerprint>& trees, size_t& asked,
                  std::vector<uint32_t>& batches) {
    const size_t end = std::min(trees.size(), asked + options_.have_batch);
    std::vector<Fingerprint> fps(trees.begin() + static_cast<ptrdiff_t>(asked),
                                 trees.begin() + static_cast<ptrdiff_t>(end));
    DDS_TRY(AcquireSlot());
    const uint32_t batch = next_batch_++;
    const auto payload = EncodeHave({batch, fps});
    report_.trees_checked += fps.size();
    window_.SentTrees(batch, std::move(fps));
    DDS_TRY(Write(MessageType::kHaveTrees, payload));
    batches.push_back(batch);
    asked = end;
    return Status::Ok();
  }

  // Waits for every batch's answers, in order.
  Result<std::vector<bool>> TreeAnswers(const std::vector<uint32_t>& batches) {
    std::vector<bool> whole;
    for (uint32_t batch : batches) {
      auto answers = window_.WaitForTrees(batch);
      DDS_TRY(answers.status());
      whole.insert(whole.end(), answers->begin(), answers->end());
    }
    return whole;
  }

  // Adds `fp` to the current HAVE batch unless already queried in this session. Returns false if
  // it was (the walk then skips what lies below it, which was queried with it).
  bool Query(const Fingerprint& fp) {
    if (queried_.count(fp) != 0) return false;
    // Bounded memory (PROTOCOL.md 4.1 step 3): forgetting may query a blob twice, never skip one.
    if (queried_.size() >= options_.queried_set_limit) queried_.clear();
    queried_.insert(fp);
    batch_.push_back(fp);
    ++report_.fingerprints_queried;
    return true;
  }

  Status MaybeSendHave() {
    while (batch_.size() >= options_.have_batch) {
      std::vector<Fingerprint> head(batch_.begin(),
                                    batch_.begin() + static_cast<ptrdiff_t>(options_.have_batch));
      batch_.erase(batch_.begin(), batch_.begin() + static_cast<ptrdiff_t>(options_.have_batch));
      DDS_TRY(SendHave(std::move(head)));
    }
    return Status::Ok();
  }

  Status FlushHave() {
    DDS_TRY(MaybeSendHave());
    if (batch_.empty()) return Status::Ok();
    return SendHave(std::exchange(batch_, {}));
  }

  // Waits for a window slot for a HAVE, sending the PUTs of HAVEs answered meanwhile first.
  Status AcquireSlot() {
    while (true) {
      switch (window_.WaitForSlotOrWork()) {
        case Window::Wake::kError:
          return window_.error();
        case Window::Wake::kMissing:
          DDS_TRY(SendMissing());
          continue;
        case Window::Wake::kSlot:
          return Status::Ok();  // only this thread fills slots, so it stays free
      }
    }
  }

  Status SendHave(std::vector<Fingerprint> fps) {
    DDS_TRY(AcquireSlot());
    const uint32_t batch = next_batch_++;
    const auto payload = EncodeHave({batch, fps});
    window_.Sent(batch, std::move(fps));
    ++report_.have_batches;
    return Write(MessageType::kHave, payload);
  }

  // Sends every answered HAVE's missing blobs, in PUTs of at most put_payload_limit bytes and
  // 65,535 blobs; a larger blob goes alone (PROTOCOL.md 3).
  Status SendMissing() {
    while (auto missing = window_.TakeMissing()) {
      Put put;
      size_t bytes = 0;
      for (const Fingerprint& fp : *missing) {
        auto blob = ReadAny(fp);
        DDS_TRY(blob.status());
        if (!put.blobs.empty() && (bytes + blob->payload.size() > options_.put_payload_limit ||
                                   put.blobs.size() == kMaxPutCount)) {
          DDS_TRY(SendPut(put));
          put.blobs.clear();
          bytes = 0;
        }
        bytes += blob->payload.size();
        put.blobs.push_back(std::move(*blob));
      }
      if (!put.blobs.empty()) DDS_TRY(SendPut(put));
    }
    return Status::Ok();
  }

  Status SendPut(Put& put) {
    // A PUT takes a slot too. Replies keep arriving while this waits, so a slot always frees.
    if (!window_.WaitForFreeSlot()) return window_.error();
    put.batch_id = next_batch_++;
    for (const PutBlob& b : put.blobs) report_.payload_bytes_sent += b.payload.size();
    report_.blobs_sent += put.blobs.size();
    ++report_.put_batches;
    window_.SentPut(put.batch_id);
    return Write(MessageType::kPut, EncodePut(put));
  }

  // A blob of known type, read under a guard (6.3).
  Result<std::vector<std::byte>> ReadBlob(const Fingerprint& fp, BlobType type) {
    ReadGuard guard(repo_.epochs);
    const auto loc = repo_.index.Lookup(fp);
    if (!loc.has_value() || loc->type != type) {
      return Status::Corruption(StrCat("sender: ", BlobTypeName(type), " blob ", fp.ToHex(),
                                       " is missing locally (run `dds verify`)"));
    }
    return repo_.containers.Read(*loc);
  }

  Result<PutBlob> ReadAny(const Fingerprint& fp) {
    ReadGuard guard(repo_.epochs);
    const auto loc = repo_.index.Lookup(fp);
    if (!loc.has_value()) {
      return Status::Aborted(StrCat("sender: blob ", fp.ToHex(), " vanished (snapshot removed?)"));
    }
    auto bytes = repo_.containers.Read(*loc);
    DDS_TRY(bytes.status());
    return PutBlob{loc->type, fp, std::move(bytes).value()};
  }

  RepoInternals& repo_;
  Connection& c_;
  const SenderOptions options_;
  SendReport& report_;
  Window window_;
  std::unordered_set<Fingerprint, FingerprintHash> queried_;
  std::vector<Fingerprint> batch_;
  uint32_t next_batch_ = 1;
  uint16_t version_ = 0;  // the session's protocol version, from HELLO_OK
};

}  // namespace

Result<SendReport> Replicate(Repository& repo, const SnapshotId& id, Connection& connection,
                             const SenderOptions& options) {
  if (options.window == 0 || options.have_batch == 0 || options.have_batch > kMaxHaveCount) {
    return Status::InvalidArgument("sender: window must be >= 1 and HAVE batches 1 to 4096");
  }
  SendReport report;
  report.id = id;
  Sender sender(repo.Internals(), connection, options, report);
  const Status hello = sender.Hello();
  if (!hello.ok()) {
    connection.Close();
    return hello;
  }
  std::thread reader([&sender] { sender.ReadReplies(); });
  auto committed = sender.Send(id);
  if (!committed.ok()) sender.window().Fail(committed.status());
  connection.Close();  // ends the reader's wait, whatever state it is in
  reader.join();
  DDS_TRY(committed.status());
  if (*committed != id) return Status::Corruption("COMMIT_OK names a different snapshot");
  return report;
}

}  // namespace dds
