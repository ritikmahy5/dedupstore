// Backup and restore (ARCHITECTURE.md 4.1, 4.2).

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "dds/common/encoding.h"
#include "dds/fs/mem_fs.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/restore_sink.h"
#include "dds/snapshot/source_tree.h"
#include "support/random_data.h"
#include "support/repo_fixture.h"
#include "support/sample_trees.h"
#include "support/temp_dir.h"
#include "support/tree_compare.h"

namespace dds {
namespace {

using testing::EdgeCaseTree;
using testing::Text;

class BackupRestoreTest : public testing::RepoTest {};

TEST_F(BackupRestoreTest, RoundTripPreservesEveryEntry) {
  MemSourceTree source = EdgeCaseTree();
  const BackupResult b = BackupOk(source);
  EXPECT_EQ(b.skipped_special, 1u);
  EXPECT_EQ(b.manifest.file_count, 8u);
  EXPECT_EQ(b.manifest.directory_count, 4u);
  EXPECT_EQ(b.manifest.symlink_count, 2u);
  EXPECT_EQ(b.manifest.logical_bytes, 2048u + 65536 + 65537 + (3u << 20) + 4 + 9 + 5);
  EXPECT_EQ(b.manifest.source_path, "/src");
  const auto diffs = testing::DiffMemTrees(source.nodes(), RestoreOk(b.id));
  EXPECT_TRUE(diffs.empty()) << ::testing::PrintToString(diffs);
}

TEST_F(BackupRestoreTest, RoundTripSurvivesReopen) {
  MemSourceTree source = EdgeCaseTree();
  const BackupResult b = BackupOk(source);
  Reopen();
  EXPECT_TRUE(testing::DiffMemTrees(source.nodes(), RestoreOk(b.id)).empty());
}

TEST_F(BackupRestoreTest, UnchangedTreeSecondBackupWritesNoBlobs) {
  // The second backup writes only a new manifest.
  MemSourceTree source = EdgeCaseTree();
  const BackupResult first = BackupOk(source);
  EXPECT_GT(first.ingest.blobs_written, 0u);
  const BackupResult second = BackupOk(source);
  EXPECT_EQ(second.ingest.blobs_written, 0u);
  EXPECT_NE(second.id, first.id);
  EXPECT_EQ(second.manifest.root, first.manifest.root);  // unchanged tree, same Merkle root
  EXPECT_EQ(repo_->ListSnapshots()->size(), 2u);
}

TEST_F(BackupRestoreTest, ChangingOneFileRewritesOnlyItsPath) {
  MemSourceTree source = EdgeCaseTree();
  BackupOk(source);
  source.AddFile("d/deep/er/f", Text("changed"), 0400, 19, 0);
  const BackupResult b = BackupOk(source);
  // The new chunk, its recipe, and the three trees on its path: root, d, d/deep, d/deep/er.
  EXPECT_EQ(b.ingest.blobs_written, 1u + 1 + 4);
}

TEST_F(BackupRestoreTest, IdenticalFilesAreStoredOnce) {
  MemSourceTree source;
  const auto data = testing::RandomBytes(9, 200000);
  source.AddFile("a", data);
  source.AddFile("b", data);
  const BackupResult b = BackupOk(source);
  // Same chunks and the same recipe: the second file adds nothing but a tree entry.
  EXPECT_EQ(b.ingest.dedup_hits, b.ingest.blobs_written - 1 /*root tree*/);
}

TEST_F(BackupRestoreTest, RestoreVerifiesBlake3BeyondTheCrc) {
  // A data blob whose stored bytes have a valid CRC but are not the fingerprinted content: only
  // BLAKE3 verification (D33) can tell.
  const auto real = Text("the real content");
  const auto fake = Text("something else!!");
  const Fingerprint fp = FingerprintOf(BlobType::kData, real);
  PutRaw(fp, BlobType::kData, fake);
  Recipe recipe;
  recipe.refs = {{fp, static_cast<uint32_t>(real.size())}};
  const auto recipe_bytes = EncodeRecipe(recipe);
  const Fingerprint rfp = FingerprintOf(BlobType::kRecipe, recipe_bytes);
  PutRaw(rfp, BlobType::kRecipe, recipe_bytes);
  Tree tree;
  TreeEntry e;
  e.name = "f";
  e.mode = 0644;
  e.size = real.size();
  e.pieces = {rfp};
  tree.entries = {e};
  const auto tree_bytes = EncodeTree(tree).value();
  const Fingerprint tfp = FingerprintOf(BlobType::kTree, tree_bytes);
  PutRaw(tfp, BlobType::kTree, tree_bytes);
  const SnapshotId id = CommitRoot(tfp);

  MemRestoreSink sink;
  auto verified = repo_->Restore(id, sink, RestoreOptions{});
  EXPECT_EQ(verified.status().code(), StatusCode::kCorruption);
  EXPECT_NE(verified.status().message().find("does not hash"), std::string::npos)
      << verified.status().message();
  // --no-verify restores whatever the container holds, which is what verification exists to stop.
  EXPECT_EQ(RestoreOk(id, /*verify=*/false).at("f").content, fake);
}

TEST_F(BackupRestoreTest, MissingBlobIsCorruptionNamingThePath) {
  // Shorter than min_size, so the whole file is one chunk.
  const auto data = testing::RandomBytes(5, 1000);
  MemSourceTree source;
  source.AddDir("d");
  source.AddFile("d/f", data);
  const BackupResult b = BackupOk(source);
  const Fingerprint fp = FingerprintOf(BlobType::kData, data);
  const auto data_loc = repo_->Internals().index.Lookup(fp);
  ASSERT_TRUE(data_loc.has_value());
  // Drop the data chunk's index entry, as if its container had been lost.
  ASSERT_EQ(repo_->Internals().index.RemoveIfDead(fp, *data_loc, 1, ~Epoch{0}),
            RemoveResult::kRemoved);
  MemRestoreSink sink;
  auto restored = repo_->Restore(b.id, sink, RestoreOptions{});
  EXPECT_EQ(restored.status().code(), StatusCode::kCorruption);
  EXPECT_NE(restored.status().message().find("d/f"), std::string::npos)
      << restored.status().message();
}

// A MemSourceTree that fails to open one file, or to list one directory, with IoError. kNone
// matches no path ("" would match the root).
constexpr const char* kNone = "<none>";

class FailingSourceTree : public SourceTree {
 public:
  FailingSourceTree(MemTree nodes, std::string bad_file, std::string bad_dir)
      : inner_(std::move(nodes)), bad_file_(std::move(bad_file)), bad_dir_(std::move(bad_dir)) {}
  Result<std::vector<SourceEntry>> ListDir(const std::string& path) override {
    if (path == bad_dir_) return Status::IoError(path + ": cannot list (injected)");
    return inner_.ListDir(path);
  }
  Result<std::unique_ptr<ByteSource>> OpenFile(const std::string& path) override {
    if (path == bad_file_) return Status::IoError(path + ": cannot open (injected)");
    return inner_.OpenFile(path);
  }
  Result<std::string> ReadLink(const std::string& path) override { return inner_.ReadLink(path); }

 private:
  MemSourceTree inner_;
  const std::string bad_file_;
  const std::string bad_dir_;
};

MemTree ManyFiles(int count = 300) {
  MemSourceTree t;
  t.AddDir("a");
  t.AddDir("z");
  for (int i = 0; i < count; ++i) {
    const std::string dir = i < count / 2 ? "a/" : "z/";
    t.AddFile(dir + std::to_string(1000 + i), testing::RandomBytes(static_cast<uint64_t>(i), 3000));
  }
  return t.nodes();
}

TEST_F(BackupRestoreTest, AWorkerFailureFailsTheBackupAndCommitsNothing) {
  // D79: with 4 workers the queue holds 16 batches of at most 64 files (D102), so with 2000 files
  // the walker is usually blocked on a full queue when the failure happens; the backup must still
  // return the worker's error, not hang, and commit nothing. The repository stays usable.
  FailingSourceTree source(ManyFiles(2000), "a/1100", kNone);
  BackupOptions options;
  options.threads = 4;
  auto failed = repo_->Backup(source, options);
  EXPECT_EQ(failed.status().code(), StatusCode::kIoError) << failed.status().ToString();
  EXPECT_NE(failed.status().message().find("a/1100"), std::string::npos)
      << failed.status().ToString();
  EXPECT_TRUE(repo_->ListSnapshots()->empty());
  MemSourceTree good(ManyFiles());
  auto ok = repo_->Backup(good, options);
  ASSERT_TRUE(ok.ok()) << ok.status().ToString();
  EXPECT_TRUE(testing::DiffMemTrees(good.nodes(), RestoreOk(ok->id)).empty());
}

TEST_F(BackupRestoreTest, AWalkerFailureStopsTheWorkersAndCommitsNothing) {
  FailingSourceTree source(ManyFiles(), kNone, "z");  // "a" is queued and stored first
  BackupOptions options;
  options.threads = 4;
  auto failed = repo_->Backup(source, options);
  EXPECT_EQ(failed.status().code(), StatusCode::kIoError) << failed.status().ToString();
  EXPECT_TRUE(repo_->ListSnapshots()->empty());
}

TEST_F(BackupRestoreTest, ZeroThreadsIsInvalidArgument) {
  MemSourceTree source;
  BackupOptions options;
  options.threads = 0;
  EXPECT_EQ(repo_->Backup(source, options).status().code(), StatusCode::kInvalidArgument);
}

// A MemRestoreSink that runs `on_create_file` before creating each file, to interleave another
// operation with a restore at a known point.
class InterleavingSink : public RestoreSink {
 public:
  explicit InterleavingSink(std::function<void()> on_create_file)
      : on_create_file_(std::move(on_create_file)) {}
  Result<DirHandle> OpenRoot() override { return inner_.OpenRoot(); }
  Result<DirHandle> MakeDir(DirHandle parent, std::string_view name) override {
    return inner_.MakeDir(parent, name);
  }
  Result<std::unique_ptr<FileSink>> CreateFile(DirHandle parent, std::string_view name) override {
    on_create_file_();
    return inner_.CreateFile(parent, name);
  }
  Status MakeSymlink(DirHandle parent, std::string_view name, std::string_view target,
                     const EntryMeta& meta) override {
    return inner_.MakeSymlink(parent, name, target, meta);
  }
  Status FinishDir(DirHandle dir, const std::optional<EntryMeta>& meta) override {
    return inner_.FinishDir(dir, meta);
  }

 private:
  std::function<void()> on_create_file_;
  MemRestoreSink inner_;
};

TEST_F(BackupRestoreTest, SnapshotRemovedDuringRestoreIsAbortedNotCorruption) {
  // ARCHITECTURE.md 4.2: in a serving process, `rm` and then gc can remove a snapshot's blobs
  // while it is being restored. That is Aborted (exit 6), not damage.
  const auto data = testing::RandomBytes(6, 1000);  // one chunk
  MemSourceTree source;
  source.AddFile("f", data);
  const BackupResult b = BackupOk(source);
  const Fingerprint fp = FingerprintOf(BlobType::kData, data);
  InterleavingSink sink([&] {
    ASSERT_TRUE(repo_->RemoveSnapshot(b.id).ok());
    const auto loc = repo_->Internals().index.Lookup(fp);
    ASSERT_TRUE(loc.has_value());
    ASSERT_EQ(repo_->Internals().index.RemoveIfDead(fp, *loc, 1, ~Epoch{0}),
              RemoveResult::kRemoved);
  });
  auto restored = repo_->Restore(b.id, sink, RestoreOptions{});
  EXPECT_EQ(restored.status().code(), StatusCode::kAborted) << restored.status().ToString();
}

TEST_F(BackupRestoreTest, ListPathWalksTheSnapshot) {
  MemSourceTree source = EdgeCaseTree();
  const BackupResult b = BackupOk(source);
  auto root = repo_->ListPath(b.id, "");
  ASSERT_TRUE(root.ok());
  EXPECT_EQ(root->size(), 6u);  // the fifo was skipped
  auto deep = repo_->ListPath(b.id, "d/deep/er");
  ASSERT_TRUE(deep.ok());
  ASSERT_EQ(deep->size(), 1u);
  EXPECT_EQ((*deep)[0].name, "f");
  EXPECT_EQ(repo_->ListPath(b.id, "d/nope").status().code(), StatusCode::kNotFound);
  EXPECT_EQ(repo_->ListPath(b.id, "link").status().code(), StatusCode::kNotFound);
}

TEST_F(BackupRestoreTest, RemovedSnapshotIsGone) {
  MemSourceTree source = EdgeCaseTree();
  const BackupResult b = BackupOk(source);
  ASSERT_TRUE(repo_->RemoveSnapshot(b.id).ok());
  MemRestoreSink sink;
  EXPECT_EQ(repo_->Restore(b.id, sink, RestoreOptions{}).status().code(), StatusCode::kNotFound);
  EXPECT_TRUE(repo_->ListSnapshots()->empty());
}

// Restores a hostile snapshot whose root holds one entry named `name` into a fresh directory and
// checks that it fails as corruption and creates nothing outside the destination (G10).
class HostileRestoreTest : public BackupRestoreTest {};

std::vector<std::byte> RawTreeWithDirNamed(std::string_view name, const Fingerprint& subtree) {
  Writer w;
  w.U8(1);
  w.U32(1);
  w.Str16(name);
  w.U8(2);  // directory
  w.U32(0755);
  w.I64(0);
  w.U32(0);
  w.Bytes(subtree.AsBytes());
  return w.Take();
}

TEST_F(HostileRestoreTest, NamesThatClimbOutAreRejected) {
  // A tree containing "..", and a tree containing a name with '/'.
  Tree inner;
  TreeEntry file;
  file.name = "planted";
  file.mode = 0644;
  inner.entries = {file};
  const auto inner_bytes = EncodeTree(inner).value();
  const Fingerprint inner_fp = FingerprintOf(BlobType::kTree, inner_bytes);
  PutRaw(inner_fp, BlobType::kTree, inner_bytes);
  for (std::string_view name : {"..", "../escape", "a/b", "."}) {
    const auto root = RawTreeWithDirNamed(name, inner_fp);
    const Fingerprint root_fp = FingerprintOf(BlobType::kTree, root);
    PutRaw(root_fp, BlobType::kTree, root);
    const SnapshotId id = CommitRoot(root_fp);
    testing::TempDir dir;
    std::filesystem::create_directory(dir.Sub("dest"));
    PosixRestoreSink sink(dir.Sub("dest"));
    auto restored = repo_->Restore(id, sink, RestoreOptions{});
    EXPECT_EQ(restored.status().code(), StatusCode::kCorruption) << name;
    // Nothing was created anywhere: not in the destination, not next to it.
    EXPECT_TRUE(std::filesystem::is_empty(dir.Sub("dest"))) << name;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir.path()), {}), 1) << name;
  }
}

TEST_F(HostileRestoreTest, TreeThatContainsItselfIsCorruptionNotARecursion) {
  // The index claims fingerprint X is a tree whose one directory entry is X again. Honest content
  // cannot do this, but without hash checks nothing else stops the walk.
  Fingerprint x;
  x.bytes.fill(0x5A);
  PutRaw(x, BlobType::kTree, RawTreeWithDirNamed("loop", x));
  const SnapshotId id = CommitRoot(x);
  MemRestoreSink verified_sink;
  auto verified = repo_->Restore(id, verified_sink, RestoreOptions{});
  EXPECT_EQ(verified.status().code(), StatusCode::kCorruption);
  EXPECT_NE(verified.status().message().find("does not hash"), std::string::npos);
  MemRestoreSink unverified_sink;
  RestoreOptions no_verify;
  no_verify.verify = false;
  auto unverified = repo_->Restore(id, unverified_sink, no_verify);
  EXPECT_EQ(unverified.status().code(), StatusCode::kCorruption);
  EXPECT_NE(unverified.status().message().find("contains itself"), std::string::npos)
      << unverified.status().message();
  auto report = repo_->Verify(VerifyOptions{});
  ASSERT_TRUE(report.ok());
  ASSERT_EQ(report->errors.size(), 1u);
  EXPECT_NE(report->errors[0].find("loop: Corruption: tree"), std::string::npos)
      << report->errors[0];
}

bool IsCaseInsensitive(const std::string& dir) {
  std::ofstream(dir + "/CaseProbe") << "x";
  const bool insensitive = std::filesystem::exists(dir + "/caseprobe");
  std::filesystem::remove(dir + "/CaseProbe");
  return insensitive;
}

TEST_F(HostileRestoreTest, CaseInsensitiveSymlinkTrickFailsWithAlreadyExists) {
  // A symlink "L" pointing outside the destination, then a directory "l" holding a
  // file. On a case-insensitive volume "l" names the symlink; the restore must fail on "l" with
  // AlreadyExists instead of writing the file through "L".
  testing::TempDir dir;
  if (!IsCaseInsensitive(dir.path())) GTEST_SKIP() << "needs a case-insensitive volume (macOS)";
  std::filesystem::create_directory(dir.Sub("outside"));
  Tree inner;
  TreeEntry file;
  file.name = "planted";
  file.mode = 0644;
  inner.entries = {file};
  const auto inner_bytes = EncodeTree(inner).value();
  const Fingerprint inner_fp = FingerprintOf(BlobType::kTree, inner_bytes);
  PutRaw(inner_fp, BlobType::kTree, inner_bytes);
  Tree root;
  TreeEntry link;
  link.name = "L";
  link.kind = EntryKind::kSymlink;
  link.mode = 0777;
  link.target = dir.Sub("outside");
  TreeEntry sub;
  sub.name = "l";
  sub.kind = EntryKind::kDirectory;
  sub.mode = 0755;
  sub.subtree = inner_fp;
  root.entries = {link, sub};  // "L" (0x4c) sorts before "l" (0x6c)
  const auto root_bytes = EncodeTree(root).value();
  const Fingerprint root_fp = FingerprintOf(BlobType::kTree, root_bytes);
  PutRaw(root_fp, BlobType::kTree, root_bytes);
  const SnapshotId id = CommitRoot(root_fp);
  PosixRestoreSink sink(dir.Sub("dest"));
  auto restored = repo_->Restore(id, sink, RestoreOptions{});
  EXPECT_EQ(restored.status().code(), StatusCode::kAlreadyExists);
  EXPECT_NE(restored.status().message().find("'L'"), std::string::npos)
      << restored.status().message();
  EXPECT_TRUE(std::filesystem::is_empty(dir.Sub("outside")));
}

TEST_F(BackupRestoreTest, ReadOnlyDirectoryRestoresWithItsContents) {
  // A 0555 directory with files restores its contents and ends 0555.
  testing::TempDir dir;
  std::filesystem::create_directory(dir.Sub("src"));
  std::filesystem::create_directory(dir.Sub("src/ro"));
  std::ofstream(dir.Sub("src/ro/a")) << "alpha";
  std::ofstream(dir.Sub("src/ro/b")) << "beta";
  ASSERT_EQ(::chmod(dir.Sub("src/ro").c_str(), 0555), 0);
  PosixSourceTree source(dir.Sub("src"));
  const BackupResult b = BackupOk(source);
  PosixRestoreSink sink(dir.Sub("dest"));
  auto restored = repo_->Restore(b.id, sink, RestoreOptions{});
  ASSERT_TRUE(restored.ok()) << restored.status().ToString();
  struct stat st{};
  ASSERT_EQ(::stat(dir.Sub("dest/ro").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 0555u);
  const auto diffs = testing::DiffMemTrees(testing::ReadDirectory(dir.Sub("src")),
                                           testing::ReadDirectory(dir.Sub("dest")));
  EXPECT_TRUE(diffs.empty()) << ::testing::PrintToString(diffs);
}

}  // namespace
}  // namespace dds
