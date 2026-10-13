#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <span>

#include "dds/fs/mem_fs.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/restore_sink.h"
#include "dds/snapshot/source_tree.h"
#include "support/fake_process_identity.h"

namespace dds::testing {

// A fresh MemFs repository with a manual clock, opened as one fake process, plus helpers for the
// common steps of repository-level tests.
class RepoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Repository::Init(fs_, "/r", RepoParams{}).ok());
    Reopen();
  }

  void Reopen() {
    repo_.reset();
    OpenOptions options;
    options.clock = &clock_;
    auto repo = Repository::Open(fs_, "/r", me_, options);
    ASSERT_TRUE(repo.ok()) << repo.status().ToString();
    repo_ = std::move(repo).value();
  }

  BackupResult BackupOk(SourceTree& source) {
    clock_.Advance(1000);  // distinct created_at, so distinct snapshot ids
    BackupOptions options;
    options.source_path = "/src";
    auto result = repo_->Backup(source, options);
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    return result.ok() ? *result : BackupResult{};
  }

  MemTree RestoreOk(const SnapshotId& id, bool verify = true) {
    MemRestoreSink sink;
    RestoreOptions options;
    options.verify = verify;
    auto result = repo_->Restore(id, sink, options);
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    return sink.nodes();
  }

  // A sealed, published blob written straight into the repository, bypassing the backup code, the
  // way a corrupted or hostile repository could hold it.
  void PutRaw(const Fingerprint& fp, BlobType type, std::span<const std::byte> payload) {
    RepoInternals& in = repo_->Internals();
    ContainerWriter w = in.NewWriter();
    w.Add(fp, type, payload);
    auto sealed = w.Seal();
    ASSERT_TRUE(sealed.ok());
    in.PublishSealed(*sealed, in.epochs.Current());
  }

  SnapshotId CommitRoot(const Fingerprint& root) {
    clock_.Advance(1000);
    Manifest m;
    m.origin_repo_id = repo_->config().repo_id;
    m.root = root;
    m.created_at_ns = clock_.WallNanos();
    auto id = repo_->Internals().snapshots.Commit(EncodeManifest(m));
    EXPECT_TRUE(id.ok());
    return id.ok() ? *id : SnapshotId{};
  }

  MemFs fs_;
  ManualClock clock_{1700000000000000000, 1};
  FakeProcessIdentity me_{"h", 1, 1};
  std::unique_ptr<Repository> repo_;
};

}  // namespace dds::testing
