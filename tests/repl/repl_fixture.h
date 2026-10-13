#pragma once

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <thread>

#include "dds/fs/mem_fs.h"
#include "dds/repl/receiver.h"
#include "dds/repl/sender.h"
#include "dds/repl/simulated_link.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/restore_sink.h"
#include "dds/snapshot/source_tree.h"
#include "dds/snapshot/tree.h"
#include "support/fake_process_identity.h"
#include "support/tree_compare.h"

namespace dds::testing {

// A MemFs repository opened by its own fake process.
struct ReplRepo {
  explicit ReplRepo(RepoParams params = {}) {
    EXPECT_TRUE(Repository::Init(fs, "/r", params).ok());
    Open();
  }
  void Open() {
    repo.reset();
    auto opened = Repository::Open(fs, "/r", self);
    EXPECT_TRUE(opened.ok()) << opened.status().ToString();
    repo = std::move(opened).value();
  }
  SnapshotId Backup(const MemTree& tree) {
    MemSourceTree source(tree);
    auto b = repo->Backup(source, BackupOptions{});
    EXPECT_TRUE(b.ok()) << b.status().ToString();
    return b.ok() ? b->id : SnapshotId{};
  }
  bool Restores(const SnapshotId& id, const MemTree& tree) {
    MemRestoreSink sink;
    auto restored = repo->Restore(id, sink, RestoreOptions{});
    if (!restored.ok()) return false;
    return DiffMemTrees(tree, sink.nodes()).empty();
  }
  bool Verifies() {
    VerifyOptions deep;
    deep.deep = true;
    auto report = repo->Verify(deep);
    return report.ok() && report->ok();
  }

  MemFs fs;
  FakeProcessIdentity self{"h", 1, 1};
  std::unique_ptr<Repository> repo;
};

struct ReplResult {
  Result<SendReport> sent = Status::Aborted("not run");
  Result<ReceiveReport> received = Status::Aborted("not run");
};

// Runs one replication of `id` from `src` to `dst` over a SimulatedLink with `link`, the receiver
// on its own thread.
inline ReplResult ReplicateOverLink(ReplRepo& src, const SnapshotId& id, ReplRepo& dst,
                                    const LinkOptions& link = {}, SenderOptions sender = {}) {
  auto [a, b] = MakeSimulatedLink(link);
  ReplResult r;
  ReceiverOptions receiver;
  receiver.idle_timeout = std::chrono::milliseconds(10000);
  sender.reply_timeout = std::chrono::milliseconds(10000);
  std::thread serve(
      [&, end = b.get()] { r.received = ServeReplication(*dst.repo, *end, receiver); });
  r.sent = Replicate(*src.repo, id, *a, sender);
  a->Close();
  serve.join();
  return r;
}

// Every blob reachable from snapshot `id` of `repo`, with its length.
inline std::map<Fingerprint, uint32_t> Reachable(ReplRepo& r, const SnapshotId& id) {
  std::map<Fingerprint, uint32_t> out;
  RepoInternals& in = r.repo->Internals();
  std::vector<Fingerprint> trees = {in.snapshots.Read(id)->root};
  while (!trees.empty()) {
    const Fingerprint fp = trees.back();
    trees.pop_back();
    const Location loc = *in.index.Lookup(fp);
    if (!out.emplace(fp, loc.length).second) continue;
    auto tree = DecodeTree(*in.containers.Read(loc));
    for (const TreeEntry& e : tree->entries) {
      if (e.kind == EntryKind::kDirectory) trees.push_back(e.subtree);
      if (e.kind != EntryKind::kFile) continue;
      for (const Fingerprint& piece : e.pieces) {
        const Location ploc = *in.index.Lookup(piece);
        out.emplace(piece, ploc.length);
        auto recipe = DecodeRecipe(*in.containers.Read(ploc), 65536);
        for (const ChunkRef& ref : recipe->refs) out.emplace(ref.fp, ref.length);
      }
    }
  }
  return out;
}

}  // namespace dds::testing
