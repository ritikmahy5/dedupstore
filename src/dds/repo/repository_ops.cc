// The snapshot operations of Repository. Each delegates to the component that implements it, so
// this file only resolves arguments and shapes results.

#include <algorithm>

#include "dds/common/strings.h"
#include "dds/repo/blob_reader.h"
#include "dds/repo/repository.h"

namespace dds {

namespace {

// "a//b/" -> {"a", "b"}. Empty components are skipped, so "" and "/" both name the root.
std::vector<std::string_view> PathComponents(std::string_view path) {
  std::vector<std::string_view> out;
  while (!path.empty()) {
    const size_t slash = path.find('/');
    const std::string_view part = path.substr(0, slash);
    if (!part.empty()) out.push_back(part);
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
  return out;
}

}  // namespace

Result<BackupResult> Repository::Backup(SourceTree& source, const BackupOptions& options) {
  return RunBackup(*internals_, source, options, internals_->clock);
}

Result<RestoreResult> Repository::Restore(const SnapshotId& id, RestoreSink& sink,
                                          const RestoreOptions& options) {
  return RunRestore(*internals_, id, sink, options);
}

Result<std::vector<SnapshotInfo>> Repository::ListSnapshots() {
  auto listing = internals_->snapshots.List();
  DDS_TRY(listing.status());
  std::vector<SnapshotInfo> out;
  for (const SnapshotId& id : listing->ids) {
    auto manifest = internals_->snapshots.Read(id);
    if (manifest.status().code() == StatusCode::kNotFound) continue;  // removed since listing
    DDS_TRY(manifest.status());
    out.push_back({id, std::move(manifest).value()});
  }
  std::sort(out.begin(), out.end(), [](const SnapshotInfo& a, const SnapshotInfo& b) {
    if (a.manifest.created_at_ns != b.manifest.created_at_ns) {
      return a.manifest.created_at_ns < b.manifest.created_at_ns;
    }
    return a.id < b.id;
  });
  return out;
}

Result<SnapshotId> Repository::ResolveSnapshot(std::string_view id_or_prefix) {
  return internals_->snapshots.Resolve(id_or_prefix);
}

Result<std::vector<TreeEntry>> Repository::ListPath(const SnapshotId& id, std::string_view path) {
  auto manifest = internals_->snapshots.Read(id);
  DDS_TRY(manifest.status());
  const std::vector<std::string_view> names = PathComponents(path);
  BlobReader reader(*internals_, /*verify_hash=*/true);
  Fingerprint dir = manifest->root;
  std::string walked = "/";
  for (size_t depth = 0;; ++depth) {
    auto tree = [&] {
      ReadGuard guard(internals_->epochs);  // one tree blob per guard (6.3)
      return reader.ReadTree(dir);
    }();
    // A blob the snapshot needs is damage, not a wrong path (exit code 3, not 2).
    if (tree.status().code() == StatusCode::kNotFound) {
      return Status::Corruption(StrCat(walked, ": ", tree.status().message()));
    }
    DDS_TRY(tree.status());
    if (depth == names.size()) return std::move(tree->entries);
    const std::string_view name = names[depth];
    walked = depth == 0 ? std::string(name) : StrCat(walked, "/", name);
    const auto it = std::find_if(tree->entries.begin(), tree->entries.end(),
                                 [&](const TreeEntry& e) { return e.name == name; });
    if (it == tree->entries.end()) {
      return Status::NotFound(StrCat(walked, ": no such entry in snapshot ", SnapshotIdToHex(id)));
    }
    if (it->kind != EntryKind::kDirectory) {
      return Status::NotFound(StrCat(walked, ": not a directory"));
    }
    dir = it->subtree;
  }
}

Status Repository::RemoveSnapshot(const SnapshotId& id) { return internals_->snapshots.Remove(id); }

Result<VerifyReport> Repository::Verify(const VerifyOptions& options) {
  return RunVerify(*internals_, options);
}

Result<RepoStats> Repository::Stats() { return ComputeStats(*internals_); }

}  // namespace dds
