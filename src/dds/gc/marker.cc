#include "dds/gc/marker.h"

#include "dds/common/strings.h"
#include "dds/repo/repository.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/tree.h"

namespace dds {

namespace {

class Marker {
 public:
  Marker(RepoInternals& repo, GcRun run, MarkCounters& counters)
      : repo_(repo), run_(run), counters_(counters) {}

  // Marks the tree at `root` and everything below it. Iterative, so a deep tree cannot exhaust the
  // stack of GC's thread.
  Status MarkTree(const Fingerprint& root) {
    std::vector<Fingerprint> stack = {root};
    while (!stack.empty()) {
      const Fingerprint fp = stack.back();
      stack.pop_back();
      auto marked = Mark(fp, BlobType::kTree);
      DDS_TRY(marked.status());
      if (!marked->first) continue;  // walked already in this run
      auto bytes = repo_.containers.Read(marked->loc);
      DDS_TRY(bytes.status());
      auto tree = DecodeTree(*bytes);
      if (!tree.ok()) return tree.status().WithContext(StrCat("tree ", fp.ToHex()));
      for (const TreeEntry& e : tree->entries) {
        if (e.kind == EntryKind::kDirectory) stack.push_back(e.subtree);
        if (e.kind != EntryKind::kFile) continue;
        for (const Fingerprint& piece : e.pieces) DDS_TRY(MarkPiece(piece));
      }
    }
    return Status::Ok();
  }

 private:
  Status MarkPiece(const Fingerprint& fp) {
    auto marked = Mark(fp, BlobType::kRecipe);
    DDS_TRY(marked.status());
    if (!marked->first) return Status::Ok();
    auto bytes = repo_.containers.Read(marked->loc);
    DDS_TRY(bytes.status());
    auto recipe = DecodeRecipe(*bytes, repo_.config.chunker.max_size);
    if (!recipe.ok()) return recipe.status().WithContext(StrCat("recipe ", fp.ToHex()));
    for (const ChunkRef& ref : recipe->refs) DDS_TRY(Mark(ref.fp, BlobType::kData).status());
    return Status::Ok();
  }

  // Marks one blob, checking that it exists with the type it is referenced as.
  Result<MarkResult> Mark(const Fingerprint& fp, BlobType type) {
    auto marked = repo_.index.MarkLive(fp, run_);
    if (!marked.has_value()) {
      return Status::Corruption(StrCat("gc: reachable ", BlobTypeName(type), " blob ", fp.ToHex(),
                                       " is missing; nothing was deleted (run `dds verify`)"));
    }
    if (marked->loc.type != type) {
      return Status::Corruption(StrCat("gc: blob ", fp.ToHex(), " is a ",
                                       BlobTypeName(marked->loc.type), " blob, referenced as a ",
                                       BlobTypeName(type)));
    }
    if (marked->first) ++counters_.blobs;
    return *marked;
  }

  RepoInternals& repo_;
  const GcRun run_;
  MarkCounters& counters_;
};

}  // namespace

Status MarkAll(RepoInternals& repo, GcRun run, const std::vector<SnapshotId>& snapshots,
               MarkCounters& counters) {
  Marker marker(repo, run, counters);
  for (const SnapshotId& id : snapshots) {
    auto manifest = repo.snapshots.Read(id);
    // Removed after the listing (a concurrent rm): its blobs may go (4.3 step 2).
    if (manifest.status().code() == StatusCode::kNotFound) continue;
    DDS_TRY(manifest.status());
    const Status marked = marker.MarkTree(manifest->root);
    if (!marked.ok()) return marked.WithContext(StrCat("snapshot ", SnapshotIdToHex(id)));
    ++counters.snapshots;
  }
  return Status::Ok();
}

}  // namespace dds
