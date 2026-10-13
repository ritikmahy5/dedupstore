// `verify` (SPEC.md section 5) and `stats` (SPEC.md sections 1 and 3).

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "dds/common/strings.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/recipe.h"
#include "support/file_damage.h"
#include "support/random_data.h"
#include "support/repo_fixture.h"
#include "support/sample_trees.h"

namespace dds {
namespace {

using testing::EdgeCaseTree;
using testing::RandomBytes;
using testing::Text;

class VerifyTest : public testing::RepoTest {
 protected:
  VerifyReport VerifyOk(bool deep) {
    VerifyOptions options;
    options.deep = deep;
    auto report = repo_->Verify(options);
    EXPECT_TRUE(report.ok()) << report.status().ToString();
    return report.ok() ? *report : VerifyReport{};
  }
  bool Mentions(const VerifyReport& report, const std::string& text) {
    for (const std::string& e : report.errors) {
      if (e.find(text) != std::string::npos) return true;
    }
    return false;
  }
};

TEST_F(VerifyTest, CleanRepositoryPasses) {
  MemSourceTree source = EdgeCaseTree();
  BackupOk(source);
  source.AddFile("d/deep/er/f", Text("changed"), 0400, 19, 0);
  BackupOk(source);
  const VerifyReport report = VerifyOk(/*deep=*/true);
  EXPECT_TRUE(report.ok()) << ::testing::PrintToString(report.errors);
  EXPECT_TRUE(report.warnings.empty());
  EXPECT_EQ(report.snapshots, 2u);
  EXPECT_EQ(report.containers, repo_->Internals().containers.PublishedTotals().containers);
  EXPECT_EQ(report.reachable_blobs, repo_->Internals().index.Size());
  EXPECT_EQ(report.unreachable_blobs, 0u);
  EXPECT_EQ(report.duplicate_copies, 0u);
  EXPECT_GT(report.bytes_hashed, uint64_t{3} << 20);  // at least d/big
}

TEST_F(VerifyTest, UnreachableBlobsAreSpaceNotErrors) {
  MemSourceTree source = EdgeCaseTree();
  const BackupResult first = BackupOk(source);
  source.AddFile("d/deep/er/f", Text("changed"), 0400, 19, 0);
  BackupOk(source);
  ASSERT_TRUE(repo_->RemoveSnapshot(first.id).ok());
  const VerifyReport report = VerifyOk(/*deep=*/false);
  EXPECT_TRUE(report.ok()) << ::testing::PrintToString(report.errors);
  // The old version's chunk and recipe, and the four trees on its path.
  EXPECT_EQ(report.unreachable_blobs, 6u);
  EXPECT_GT(report.unreachable_bytes, 0u);
}

// A small tree in which every blob belongs to exactly one path, so a damaged blob has one right
// answer for "which path".
MemSourceTree OneBlobPerPathTree() {
  MemSourceTree t;
  t.AddFile("a", RandomBytes(1, 100));  // each file is below min_size: one chunk, one recipe
  t.AddDir("d");
  t.AddFile("d/b", RandomBytes(2, 200));
  t.AddDir("d/e");
  t.AddFile("d/e/c", RandomBytes(3, 300));
  t.AddDir("d/z");  // the only empty directory, so the only empty tree
  t.AddSymlink("s", "a");
  return t;
}

// Fingerprint -> the path verify must name for it ("/" for the root tree).
std::map<Fingerprint, std::string> PathOfEveryBlob(Repository& repo, const SnapshotId& id,
                                                   const MemSourceTree& source) {
  std::map<Fingerprint, std::string> out;
  out[repo.Internals().snapshots.Read(id)->root] = "/";
  for (const char* dir : {"", "d", "d/e", "d/z"}) {
    auto entries = repo.ListPath(id, dir);
    EXPECT_TRUE(entries.ok());
    for (const TreeEntry& e : *entries) {
      const std::string path = ChildPath(dir, e.name);
      if (e.kind == EntryKind::kDirectory) out[e.subtree] = path;
      if (e.kind == EntryKind::kFile) {
        out[e.pieces.at(0)] = path;
        out[FingerprintOf(BlobType::kData, source.nodes().at(path).content)] = path;
      }
    }
  }
  return out;
}

TEST_F(VerifyTest, DeepVerifyFindsAFlippedBitInAnyReachableBlob) {
  // verify --deep detects a flipped bit in any reachable blob and names the snapshot
  // and the path. Both snapshots share every blob, so both must be named.
  MemSourceTree source = OneBlobPerPathTree();
  const BackupResult first = BackupOk(source);
  const BackupResult second = BackupOk(source);
  const auto paths = PathOfEveryBlob(*repo_, first.id, source);
  ASSERT_EQ(paths.size(), 10u);  // 3 chunks, 3 recipes, 4 trees
  int bit = 0;
  for (const auto& [fp, path] : paths) {
    const Location loc = *repo_->Internals().index.Lookup(fp);
    const std::string file = repo_->Internals().containers.PathFor(loc.container_id);
    const size_t byte = loc.offset + loc.length / 2;
    testing::FlipBit(fs_, file, byte, bit);
    Reopen();
    const VerifyReport report = VerifyOk(/*deep=*/true);
    for (const SnapshotId& id : {first.id, second.id}) {
      EXPECT_TRUE(Mentions(report, StrCat("snapshot ", SnapshotIdToHex(id), ": ", path, ": ")))
          << path << ": " << ::testing::PrintToString(report.errors);
    }
    testing::FlipBit(fs_, file, byte, bit);  // repair
    bit = (bit + 1) % 8;
  }
  Reopen();
  EXPECT_TRUE(VerifyOk(/*deep=*/true).ok());
}

TEST_F(VerifyTest, DeepVerifyCatchesContentThatPassesItsCrc) {
  // A stored data blob with a valid CRC but the wrong content: only BLAKE3 can tell (D33).
  const auto real = Text("the real content");
  const Fingerprint fp = FingerprintOf(BlobType::kData, real);
  PutRaw(fp, BlobType::kData, Text("something else!!"));
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
  CommitRoot(tfp);
  EXPECT_TRUE(VerifyOk(/*deep=*/false).ok());  // present, right type, parses
  const VerifyReport deep = VerifyOk(/*deep=*/true);
  EXPECT_FALSE(deep.ok());
  EXPECT_TRUE(Mentions(deep, "f: Corruption:") && Mentions(deep, "does not hash"))
      << ::testing::PrintToString(deep.errors);
}

TEST_F(VerifyTest, MissingBlobIsReportedWithoutDeep) {
  MemSourceTree source = OneBlobPerPathTree();
  const BackupResult b = BackupOk(source);
  const Fingerprint fp = FingerprintOf(BlobType::kData, source.nodes().at("d/e/c").content);
  const Location loc = *repo_->Internals().index.Lookup(fp);
  ASSERT_EQ(repo_->Internals().index.RemoveIfDead(fp, loc, 1, ~Epoch{0}), RemoveResult::kRemoved);
  const VerifyReport report = VerifyOk(/*deep=*/false);
  EXPECT_TRUE(Mentions(report, StrCat("snapshot ", SnapshotIdToHex(b.id), ": d/e/c: NotFound")))
      << ::testing::PrintToString(report.errors);
}

TEST_F(VerifyTest, WrongSizeInATreeIsReported) {
  const auto data = Text("12345");
  const Fingerprint fp = FingerprintOf(BlobType::kData, data);
  PutRaw(fp, BlobType::kData, data);
  Recipe recipe;
  recipe.refs = {{fp, 5}};
  const auto recipe_bytes = EncodeRecipe(recipe);
  const Fingerprint rfp = FingerprintOf(BlobType::kRecipe, recipe_bytes);
  PutRaw(rfp, BlobType::kRecipe, recipe_bytes);
  Tree tree;
  TreeEntry e;
  e.name = "f";
  e.mode = 0644;
  e.size = 6;  // the chunks add up to 5
  e.pieces = {rfp};
  tree.entries = {e};
  const auto tree_bytes = EncodeTree(tree).value();
  const Fingerprint tfp = FingerprintOf(BlobType::kTree, tree_bytes);
  PutRaw(tfp, BlobType::kTree, tree_bytes);
  CommitRoot(tfp);
  const VerifyReport report = VerifyOk(/*deep=*/false);
  EXPECT_TRUE(Mentions(report, "f: Corruption: chunks add up to 5 bytes, the tree says 6"))
      << ::testing::PrintToString(report.errors);
}

TEST_F(VerifyTest, DamagedContainerFooterIsReportedAndItsBlobsAreMissing) {
  MemSourceTree source = OneBlobPerPathTree();
  BackupOk(source);
  const std::string file = repo_->Internals().containers.PathFor(1);
  auto size = ReadWholeFile(fs_, file);
  ASSERT_TRUE(size.ok());
  testing::FlipBit(fs_, file, size->size() - 30, 0);  // inside the footer or trailer
  Reopen();
  ASSERT_EQ(repo_->open_report().bad_containers.size(), 1u);
  const VerifyReport report = VerifyOk(/*deep=*/false);
  EXPECT_TRUE(Mentions(report, file)) << ::testing::PrintToString(report.errors);
  EXPECT_TRUE(Mentions(report, "NotFound")) << ::testing::PrintToString(report.errors);
}

TEST_F(VerifyTest, DamagedManifestIsReported) {
  MemSourceTree source = OneBlobPerPathTree();
  const BackupResult b = BackupOk(source);
  testing::FlipBit(fs_, JoinPath("/r/snapshots", ManifestFileName(b.id)), 20, 3);
  const VerifyReport report = VerifyOk(/*deep=*/false);
  EXPECT_TRUE(Mentions(report, StrCat("snapshot ", SnapshotIdToHex(b.id), ": Corruption")))
      << ::testing::PrintToString(report.errors);
}

TEST_F(VerifyTest, StrayFilesAreWarningsNotErrors) {
  for (const char* path : {"/r/containers/notes.txt", "/r/snapshots/README"}) {
    auto f = fs_.CreateNew(path);
    ASSERT_TRUE(f.ok());
    ASSERT_TRUE((*f)->Close().ok());
  }
  const VerifyReport report = VerifyOk(/*deep=*/false);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.warnings.size(), 2u);
}

class StatsTest : public testing::RepoTest {};

TEST_F(StatsTest, CountsLogicalUniqueAndPhysicalBytes) {
  MemSourceTree source;
  const auto shared = RandomBytes(7, 300000);
  source.AddFile("a", shared);
  source.AddFile("b", shared);
  source.AddFile("c", Text("small"));
  const BackupResult b = BackupOk(source);
  auto stats = repo_->Stats();
  ASSERT_TRUE(stats.ok()) << stats.status().ToString();
  EXPECT_EQ(stats->snapshots, 1u);
  EXPECT_EQ(stats->logical_bytes, 2 * 300000u + 5);
  EXPECT_EQ(stats->unique_data_bytes, 300000u + 5);
  EXPECT_DOUBLE_EQ(stats->dedup_ratio, (2 * 300000.0 + 5) / (300000.0 + 5));
  uint64_t physical = 0;
  const auto names = fs_.ListDir("/r/containers");
  ASSERT_TRUE(names.ok());
  for (const std::string& name : *names) {
    physical += ReadWholeFile(fs_, JoinPath("/r/containers", name))->size();
  }
  EXPECT_EQ(stats->physical_bytes, physical);
  EXPECT_EQ(stats->containers, 2u);  // the file writer's and the tree writer's
  EXPECT_EQ(stats->blobs, b.ingest.blobs_written);
  EXPECT_EQ(stats->duplicate_copies, 0u);
  EXPECT_GT(stats->index_memory_bytes, 0u);
  EXPECT_EQ(stats->live_fraction_histogram[9], 2u);
}

TEST_F(StatsTest, RemovedSnapshotLeavesDeadContainersAndDuplicatesAreCounted) {
  MemSourceTree one;
  one.AddFile("x", RandomBytes(1, 50000));
  MemSourceTree two;
  two.AddFile("y", RandomBytes(2, 50000));
  const BackupResult first = BackupOk(one);
  BackupOk(two);
  ASSERT_TRUE(repo_->RemoveSnapshot(first.id).ok());
  // A second copy of a live chunk, as a concurrent session could leave (D32).
  const auto chunk = RandomBytes(2, 1000);
  PutRaw(FingerprintOf(BlobType::kData, chunk), BlobType::kData, chunk);
  PutRaw(FingerprintOf(BlobType::kData, chunk), BlobType::kData, chunk);
  auto stats = repo_->Stats();
  ASSERT_TRUE(stats.ok()) << stats.status().ToString();
  EXPECT_EQ(stats->snapshots, 1u);
  EXPECT_EQ(stats->duplicate_copies, 1u);
  // First backup's two containers are dead; the second's are live; both raw containers hold an
  // unreferenced chunk, so they are dead too.
  EXPECT_EQ(stats->live_fraction_histogram[0], 4u);
  EXPECT_EQ(stats->live_fraction_histogram[9], 2u);
}

TEST_F(StatsTest, MissingBlobFailsWithCorruption) {
  MemSourceTree source;
  source.AddFile("f", Text("abc"));
  BackupOk(source);
  const Fingerprint fp = FingerprintOf(BlobType::kData, Text("abc"));
  const Location loc = *repo_->Internals().index.Lookup(fp);
  ASSERT_EQ(repo_->Internals().index.RemoveIfDead(fp, loc, 1, ~Epoch{0}), RemoveResult::kRemoved);
  EXPECT_EQ(repo_->Stats().status().code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace dds
