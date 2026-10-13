#include "dds/repo/backup_parent.h"

#include <algorithm>

#include "dds/common/log.h"
#include "dds/common/strings.h"
#include "dds/common/test_hooks.h"
#include "dds/repo/blob_reader.h"
#include "dds/repo/ingest_session.h"
#include "dds/repo/repository.h"

namespace dds {

namespace {

constexpr int64_t kNanosPerSecond = 1'000'000'000;

// Looks `fp` up the way a backup that read the file would: through the session, which stamps a
// hit. Under the mutation it only checks the index, without stamping, which the interleaving test
// must catch.
bool Present(RepoInternals& repo, IngestSession& session, const Fingerprint& fp) {
  if (DDS_MUTATION("reuse_without_stamp")) return repo.index.Lookup(fp).has_value();
  return session.Lookup(fp, Reserve::kOff) == LookupResult::kHit;
}

}  // namespace

bool UnchangedSinceParent(const SourceEntry& now, const TreeEntry& parent,
                          int64_t parent_started_at_ns) {
  if (now.kind != SourceKind::kFile || parent.kind != EntryKind::kFile) return false;
  if (now.size != parent.size || now.mtime_sec != parent.mtime_sec ||
      now.mtime_nsec != parent.mtime_nsec) {
    return false;
  }
  // Compared in whole seconds, rounded toward the past, so no nanosecond product can overflow.
  if (parent_started_at_ns <= 0) return false;
  const int64_t started_sec = parent_started_at_ns / kNanosPerSecond;
  return parent.mtime_sec < started_sec - kParentMtimeMarginSec;
}

const TreeEntry* FindEntry(const Tree& tree, std::string_view name) {
  auto it = std::lower_bound(tree.entries.begin(), tree.entries.end(), name,
                             [](const TreeEntry& e, std::string_view n) { return e.name < n; });
  return it != tree.entries.end() && it->name == name ? &*it : nullptr;
}

std::unique_ptr<Tree> ReadParentTree(RepoInternals& repo, const Fingerprint& fp) {
  ReadGuard guard(repo.epochs);
  BlobReader reader(repo, /*verify_hash=*/false);
  auto tree = reader.ReadTree(fp);
  if (!tree.ok()) {
    LogWarn(StrCat("parent tree ", fp.ToHex(), " is unreadable (", tree.status().ToString(),
                   "); reading the files below it"));
    return nullptr;
  }
  return std::make_unique<Tree>(std::move(tree).value());
}

bool ReuseParentPieces(RepoInternals& repo, IngestSession& session, const TreeEntry& parent,
                       TreeEntry& entry) {
  BlobReader reader(repo, /*verify_hash=*/false);
  uint64_t size = 0;
  for (const Fingerprint& piece : parent.pieces) {
    if (!Present(repo, session, piece)) return false;
    // A recipe this session is also storing right now is pending, not yet in the index, so the
    // read can miss; reading the file is then the safe answer.
    auto recipe = reader.ReadRecipe(piece);
    if (!recipe.ok()) return false;
    for (const ChunkRef& ref : recipe->refs) {
      if (!Present(repo, session, ref.fp)) return false;
      size += ref.length;
    }
  }
  // The parent passed verify's size rule when it was written; a mismatch now means damage.
  if (size != parent.size) return false;
  DDS_SYNC_POINT("backup.after_reuse");
  entry.size = parent.size;
  entry.pieces = parent.pieces;
  return true;
}

}  // namespace dds
