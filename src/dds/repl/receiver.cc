#include "dds/repl/receiver.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dds/common/log.h"
#include "dds/common/strings.h"
#include "dds/common/test_hooks.h"
#include "dds/repl/messages.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/tree.h"
#include "dds/store/container_format.h"

namespace dds {

namespace {

class Receiver {
 public:
  Receiver(Repository& repo, Connection& c, const ReceiverOptions& options)
      : repo_(repo.Internals()), c_(c), options_(options) {}

  ~Receiver() {
    // PROTOCOL.md 4.2 step 6: keep what was received (it is verified), so a retry need not resend.
    if (session_ != nullptr) {
      const Status sealed = session_->SealAll();
      if (!sealed.ok()) LogWarn(StrCat("replication: sealing received blobs: ", sealed.ToString()));
    }
  }

  Result<ReceiveReport> Run() {
    while (true) {
      auto frame = ReadFrame(c_, options_.idle_timeout);
      // The sender closes the connection after COMMIT_OK, or gives up: either ends this session.
      if (frame.status().code() == StatusCode::kAborted) return Finish(frame.status());
      if (!frame.ok()) return Fail(frame.status());
      const Status handled = Handle(*frame);
      if (!handled.ok()) return Fail(handled);
    }
  }

 private:
  Status Handle(const Frame& f) {
    if (session_ == nullptr && f.type != MessageType::kHello) {
      return Status::Corruption(StrCat(MessageTypeName(f.type), " before HELLO"));
    }
    switch (f.type) {
      case MessageType::kHello:
        return OnHello(f.payload);
      case MessageType::kHave:
        return OnHave(f.payload);
      case MessageType::kHaveTrees:
        return OnHaveTrees(f.payload);
      case MessageType::kPut:
        return OnPut(f.payload);
      case MessageType::kCommit:
        return OnCommit(f.payload);
      default:
        return Status::Corruption(StrCat("unexpected ", MessageTypeName(f.type), " from a sender"));
    }
  }

  Status OnHello(std::span<const std::byte> payload) {
    if (session_ != nullptr) return Status::Corruption("a second HELLO");
    auto hello = DecodeHello(payload);
    DDS_TRY(hello.status());
    if (hello->version < kOldestProtocolVersion || hello->version > kProtocolVersion) {
      return Status::Incompatible(StrCat("protocol version ", hello->version,
                                         "; this receiver speaks 1 to ", kProtocolVersion));
    }
    version_ = hello->version;  // the session speaks the sender's version (D108)
    if (ChunkingParamsDigest(hello->params_text) != hello->digest) {
      return Status::Corruption("HELLO params digest does not match its params text");
    }
    const std::string mine = ChunkingParamsText(repo_.config);
    if (ChunkingParamsDigest(mine) != hello->digest) {
      return Status::Incompatible(StrCat("chunking parameters differ; sender: [",
                                         hello->params_text, "] receiver: [", mine, "]"));
    }
    session_ = std::make_unique<IngestSession>(repo_, /*writer_count=*/1);
    DDS_TRY(WriteFrame(c_, MessageType::kHelloOk, EncodeHelloOk({version_, repo_.config.repo_id}))
                .status());
    return Status::Ok();
  }

  // The receiver's one notion of "present", shared by HAVE and the commit walk: an index hit
  // (which stamps the entry, so a concurrent GC keeps it, 6.2) or a blob this session received. A
  // miss reserves nothing (D55): the sender may never send the blob.
  LookupResult Present(const Fingerprint& fp) {
    if (checking_subtree_ && DDS_MUTATION("subtree_check_without_stamp")) {
      // Broken on purpose (ReplicationPruningTest): finds the blob without stamping it.
      return repo_.index.Lookup(fp).has_value() ? LookupResult::kHit : LookupResult::kMiss;
    }
    const Reserve reserve = DDS_MUTATION("have_miss_reserves") ? Reserve::kOn : Reserve::kOff;
    return session_->Lookup(fp, reserve);
  }

  Status OnHave(std::span<const std::byte> payload) {
    auto have = DecodeHave(payload);
    DDS_TRY(have.status());
    HaveReply reply{have->batch_id, std::vector<bool>(have->fps.size())};
    for (size_t i = 0; i < have->fps.size(); ++i) {
      reply.present[i] = Present(have->fps[i]) == LookupResult::kHit;
      report_.fingerprints_present += reply.present[i] ? 1 : 0;
    }
    report_.fingerprints_queried += have->fps.size();
    return WriteFrame(c_, MessageType::kHaveReply, EncodeHaveReply(reply)).status();
  }

  // HAVE_TREES (protocol version 2, D108): for each tree, whether it and everything below it are
  // here, which tells the sender it need not ask about anything in that subtree.
  Status OnHaveTrees(std::span<const std::byte> payload) {
    if (version_ < 2) return Status::Corruption("HAVE_TREES in a protocol version 1 session");
    auto have = DecodeHave(payload);
    DDS_TRY(have.status());
    HaveReply reply{have->batch_id, std::vector<bool>(have->fps.size())};
    for (size_t i = 0; i < have->fps.size(); ++i) {
      reply.present[i] = SubtreeWhole(have->fps[i]);
      report_.subtrees_whole += reply.present[i] ? 1 : 0;
    }
    report_.trees_checked += have->fps.size();
    return WriteFrame(c_, MessageType::kHaveTreesReply, EncodeHaveReply(reply)).status();
  }

  // Reads tree `fp` and checks it and its files' recipes and chunks as the COMMIT walk would.
  // Returns its subdirectories, or nullopt if anything is missing or bad.
  std::optional<std::vector<Fingerprint>> CheckTreeAndFiles(const Fingerprint& fp) {
    auto bytes = ReadMetadata(fp, BlobType::kTree);
    if (!bytes.ok()) return std::nullopt;
    auto tree = DecodeTree(*bytes);
    if (!tree.ok()) return std::nullopt;
    std::vector<Fingerprint> subdirs;
    for (const TreeEntry& e : tree->entries) {
      if (e.kind == EntryKind::kDirectory) subdirs.push_back(e.subtree);
      if (e.kind != EntryKind::kFile) continue;
      for (const Fingerprint& piece : e.pieces) {
        if (pieces_checked_.count(piece) != 0) continue;
        if (!WalkPiece(piece).ok()) return std::nullopt;
        pieces_checked_.insert(piece);
      }
    }
    return subdirs;
  }

  // Whether tree `root` and every blob below it are here, by the COMMIT walk's checks. Every lookup
  // goes through the session, which stamps what it finds (6.2 item 3), so nothing found here can be
  // collected before the session ends, and the COMMIT walk skips the subtrees found whole. Results
  // are remembered for the session. Post-order with an explicit stack, not recursion: a peer may
  // send a chain of trees as deep as it likes.
  bool SubtreeWhole(const Fingerprint& root) {
    checking_subtree_ = true;
    const bool whole = WalkSubtree(root);
    checking_subtree_ = false;
    return whole;
  }

  bool WalkSubtree(const Fingerprint& root) {
    struct Frame {
      Fingerprint fp;
      std::vector<Fingerprint> subdirs;
      size_t next = 0;
    };
    std::vector<Frame> stack;
    std::unordered_set<Fingerprint, FingerprintHash> on_path;
    // A known answer, or nullopt once `fp`'s frame is pushed for its subdirectories to be checked.
    auto start = [&](const Fingerprint& fp) -> std::optional<bool> {
      if (auto known = whole_.find(fp); known != whole_.end()) return known->second;
      // A tree below itself is impossible for honest content (a BLAKE3 fixed point).
      if (on_path.count(fp) != 0) return false;
      auto subdirs = CheckTreeAndFiles(fp);
      if (!subdirs.has_value()) {
        // Remembered only for a tree held here, or one named by a tree held here (the stack's
        // top): a fingerprint the peer made up is neither, and remembering each would let a peer
        // grow the memo without bound, one entry per fingerprint it sends.
        if (!stack.empty() || repo_.index.Lookup(fp).has_value()) whole_[fp] = false;
        return false;
      }
      on_path.insert(fp);
      stack.push_back({fp, std::move(*subdirs)});
      return std::nullopt;
    };
    if (const std::optional<bool> known = start(root)) return *known;
    while (true) {  // the stack is never empty here: start(root) pushed root's frame
      Frame& top = stack.back();
      if (top.next == top.subdirs.size()) {  // every subdirectory is whole: so is this tree
        whole_[top.fp] = true;
        on_path.erase(top.fp);
        stack.pop_back();
        if (stack.empty()) return true;
        continue;
      }
      // A copy: start() may push a frame and reallocate the stack under `top`.
      const Fingerprint child_fp = top.subdirs[top.next++];
      const std::optional<bool> child = start(child_fp);
      if (child.has_value() && !*child) {  // a missing descendant: every tree on the path lacks it
        for (const Frame& f : stack) whole_[f.fp] = false;
        return false;
      }
    }
  }

  Status OnPut(std::span<const std::byte> payload) {
    auto put = DecodePut(payload);
    DDS_TRY(put.status());
    for (const PutBlob& b : put->blobs) {
      const size_t limit =
          b.type == BlobType::kData ? repo_.config.chunker.max_size : kMaxMetadataBlobBytes;
      if (b.payload.size() > limit) {
        return Status::Corruption(StrCat("PUT: ", BlobTypeName(b.type), " blob of ",
                                         b.payload.size(), " bytes exceeds ", limit));
      }
      if (FingerprintOf(b.type, b.payload) != b.fp) {
        return Status::Corruption(
            StrCat("PUT: blob does not hash to its fingerprint ", b.fp.ToHex()));
      }
      report_.payload_bytes_received += b.payload.size();
      ++report_.blobs_received;
      // Verified first, then dropped if this session already has it (received or present).
      if (session_->Lookup(b.fp, Reserve::kOff) == LookupResult::kHit) {
        ++report_.blobs_dropped;
        continue;
      }
      DDS_TRY(session_->Put(b.fp, b.type, b.payload, 0));
    }
    return WriteFrame(c_, MessageType::kPutAck, EncodePutAck(put->batch_id)).status();
  }

  Status OnCommit(std::span<const std::byte> manifest_bytes) {
    auto manifest = DecodeManifest(manifest_bytes);
    DDS_TRY(manifest.status());
    const SnapshotId id = SnapshotIdOf(manifest_bytes);
    auto exists = repo_.snapshots.Exists(id);
    DDS_TRY(exists.status());
    if (!*exists) {
      // 7.3 steps 1-2, then the check that makes a hostile COMMIT harmless: every blob the
      // snapshot reaches must be here before the manifest is written.
      DDS_TRY(session_->SealAll());
      DDS_TRY(WalkSnapshot(manifest->root));
      DDS_TRY(repo_.snapshots.Commit(manifest_bytes).status());
    }
    report_.committed = id;
    return WriteFrame(c_, MessageType::kCommitOk, EncodeCommitOk(id)).status();
  }

  // Requires `fp` present with type `type`. Returns its location when the index has one.
  Result<std::optional<Location>> Require(const Fingerprint& fp, BlobType type) {
    if (Present(fp) != LookupResult::kHit) {
      return Status::Corruption(StrCat("COMMIT: the snapshot references ", BlobTypeName(type),
                                       " blob ", fp.ToHex(), ", which is not here"));
    }
    const auto loc = repo_.index.Lookup(fp);
    if (loc.has_value() && loc->type != type) {
      return Status::Corruption(StrCat("COMMIT: blob ", fp.ToHex(), " is a ",
                                       BlobTypeName(loc->type), " blob, referenced as ",
                                       BlobTypeName(type)));
    }
    return loc;
  }

  // Reads a tree or recipe for the walk: it must be present and readable.
  Result<std::vector<std::byte>> ReadMetadata(const Fingerprint& fp, BlobType type) {
    ReadGuard guard(repo_.epochs);
    auto loc = Require(fp, type);
    DDS_TRY(loc.status());
    const std::optional<Location> where = *loc;
    if (!where.has_value()) {
      return Status::Corruption(StrCat("COMMIT: blob ", fp.ToHex(), " unreadable"));
    }
    return repo_.containers.Read(*where);
  }

  Status WalkSnapshot(const Fingerprint& root) {
    std::unordered_set<Fingerprint, FingerprintHash> seen;
    std::vector<Fingerprint> trees = {root};
    while (!trees.empty()) {
      const Fingerprint fp = trees.back();
      trees.pop_back();
      if (!seen.insert(fp).second) continue;
      // Found whole by HAVE_TREES in this session: checked and stamped already (D108).
      if (auto known = whole_.find(fp); known != whole_.end() && known->second) continue;
      auto bytes = ReadMetadata(fp, BlobType::kTree);
      DDS_TRY(bytes.status());
      auto tree = DecodeTree(*bytes);
      DDS_TRY(tree.status());
      for (const TreeEntry& e : tree->entries) {
        if (e.kind == EntryKind::kDirectory) trees.push_back(e.subtree);
        if (e.kind != EntryKind::kFile) continue;
        for (const Fingerprint& piece : e.pieces) {
          if (!seen.insert(piece).second || pieces_checked_.count(piece) != 0) continue;
          DDS_TRY(WalkPiece(piece));
        }
      }
    }
    return Status::Ok();
  }

  Status WalkPiece(const Fingerprint& fp) {
    auto bytes = ReadMetadata(fp, BlobType::kRecipe);
    DDS_TRY(bytes.status());
    auto recipe = DecodeRecipe(*bytes, repo_.config.chunker.max_size);
    DDS_TRY(recipe.status());
    ReadGuard guard(repo_.epochs);  // a compressed chunk's frame header is read below
    for (const ChunkRef& ref : recipe->refs) {
      auto loc = Require(ref.fp, BlobType::kData);
      DDS_TRY(loc.status());
      const std::optional<Location> where = *loc;
      if (!where.has_value()) {
        // Received in this session but not sealed: no index entry yet, so neither its type nor its
        // length can be checked. A HAVE_TREES check runs before sealing, so the subtree is not
        // whole: skipping the chunk would let COMMIT, which skips subtrees found whole, accept a
        // wrong length or type (D108).
        if (checking_subtree_) {
          return Status::Corruption(StrCat("chunk ", ref.fp.ToHex(), " is not sealed yet"));
        }
        // COMMIT sealed everything first, and a HAVE miss reserves nothing (D55), so only a chunk
        // whose content was checked against its fingerprint on arrival gets here; mutation
        // have_miss_reserves shows that rule is what keeps it so.
        continue;
      }
      // The content length, not the stored one: this replica may compress (D107).
      auto length = repo_.containers.ContentLength(*where);
      DDS_TRY(length.status());
      if (*length != ref.length) {
        return Status::Corruption(
            StrCat("COMMIT: chunk ", ref.fp.ToHex(), " length differs from its recipe"));
      }
    }
    return Status::Ok();
  }

  // Tells the peer (best effort) and closes the connection.
  Result<ReceiveReport> Fail(const Status& status) {
    (void)WriteFrame(c_, MessageType::kError, EncodeError({status.code(), status.message()}));
    c_.Close();
    return status;
  }

  Result<ReceiveReport> Finish(const Status& ended) {
    // A close after COMMIT_OK is the normal end; before it, the transfer is incomplete.
    report_.trees_remembered = whole_.size();
    if (report_.committed.has_value() || session_ == nullptr) return report_;
    return ended;
  }

  RepoInternals& repo_;
  Connection& c_;
  const ReceiverOptions options_;
  std::unique_ptr<IngestSession> session_;
  uint16_t version_ = 0;  // the session's protocol version, from HELLO
  // What HAVE_TREES found in this session: tree -> whole or not, and recipes checked whole (D108).
  std::unordered_map<Fingerprint, bool, FingerprintHash> whole_;
  std::unordered_set<Fingerprint, FingerprintHash> pieces_checked_;
  bool checking_subtree_ = false;  // in a HAVE_TREES check: WalkPiece, and the mutation in Present
  ReceiveReport report_;
};

}  // namespace

Result<ReceiveReport> ServeReplication(Repository& repo, Connection& connection,
                                       const ReceiverOptions& options) {
  Receiver receiver(repo, connection, options);
  return receiver.Run();
}

}  // namespace dds
