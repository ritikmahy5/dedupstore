#include "dds/repo/snapshot_store.h"

#include <algorithm>

#include "dds/common/strings.h"

namespace dds {

namespace {

constexpr std::string_view kSuffix = ".snap";
constexpr size_t kMinPrefix = 6;

bool IsLowerHex(std::string_view s) {
  return std::all_of(s.begin(), s.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

std::string ManifestFileName(const SnapshotId& id) {
  return SnapshotIdToHex(id) + std::string(kSuffix);
}

std::string SnapshotStore::PathOf(const SnapshotId& id) const {
  return JoinPath(layout_.snapshots(), ManifestFileName(id));
}

Result<SnapshotStore::Listing> SnapshotStore::List() {
  MutexLock lock(mu_);
  auto names = fs_.ListDir(layout_.snapshots());
  DDS_TRY(names.status());
  Listing out;
  for (const std::string& name : *names) {
    std::optional<SnapshotId> id;
    if (name.size() == 32 + kSuffix.size() && name.ends_with(kSuffix)) {
      id = SnapshotIdFromHex(std::string_view(name).substr(0, 32));
    }
    if (id) {
      out.ids.push_back(*id);
    } else {
      out.ignored.push_back(name);
    }
  }
  std::sort(out.ids.begin(), out.ids.end());
  return out;
}

Result<std::vector<std::byte>> SnapshotStore::ReadBytes(const SnapshotId& id) {
  auto bytes = ReadWholeFile(fs_, PathOf(id));
  if (bytes.status().code() == StatusCode::kNotFound) {
    return Status::NotFound(StrCat("no snapshot ", SnapshotIdToHex(id)));
  }
  return bytes;
}

Result<Manifest> SnapshotStore::Read(const SnapshotId& id) {
  auto bytes = ReadBytes(id);
  DDS_TRY(bytes.status());
  if (SnapshotIdOf(*bytes) != id) {
    return Status::Corruption(StrCat(PathOf(id), ": content does not hash to the file name"));
  }
  auto manifest = DecodeManifest(*bytes);
  if (!manifest.ok()) return manifest.status().WithContext(PathOf(id));
  return manifest;
}

Result<bool> SnapshotStore::Exists(const SnapshotId& id) { return fs_.Exists(PathOf(id)); }

Result<SnapshotId> SnapshotStore::Commit(std::span<const std::byte> manifest_bytes) {
  const SnapshotId id = SnapshotIdOf(manifest_bytes);
  const std::string final_path = PathOf(id);
  const std::string tmp_path = JoinPath(layout_.tmp(), ManifestFileName(id) + ".tmp");
  MutexLock lock(mu_);
  auto exists = fs_.Exists(final_path);
  DDS_TRY(exists.status());
  if (*exists) return id;
  auto file = fs_.CreateNew(tmp_path);
  DDS_TRY(file.status());
  DDS_TRY((*file)->Append(manifest_bytes));
  DDS_TRY((*file)->Sync());
  DDS_TRY((*file)->Close());
  DDS_TRY(fs_.Rename(tmp_path, final_path));  // the commit point
  DDS_TRY(fs_.SyncDir(layout_.snapshots()));
  return id;
}

Status SnapshotStore::Remove(const SnapshotId& id) {
  MutexLock lock(mu_);
  Status removed = fs_.Remove(PathOf(id));
  if (removed.code() == StatusCode::kNotFound) {
    return Status::NotFound(StrCat("no snapshot ", SnapshotIdToHex(id)));
  }
  DDS_TRY(removed);
  return fs_.SyncDir(layout_.snapshots());
}

Result<SnapshotId> SnapshotStore::Resolve(std::string_view id_or_prefix) {
  if (id_or_prefix.size() < kMinPrefix || id_or_prefix.size() > 32 || !IsLowerHex(id_or_prefix)) {
    return Status::InvalidArgument(StrCat("'", id_or_prefix,
                                          "' is not a snapshot id or a prefix of at least 6 "
                                          "lowercase hex digits"));
  }
  auto listing = List();
  DDS_TRY(listing.status());
  std::vector<SnapshotId> matches;
  for (const SnapshotId& id : listing->ids) {
    if (SnapshotIdToHex(id).starts_with(id_or_prefix)) matches.push_back(id);
  }
  if (matches.empty()) return Status::NotFound(StrCat("no snapshot matches '", id_or_prefix, "'"));
  if (matches.size() > 1) {
    return Status::InvalidArgument(
        StrCat("'", id_or_prefix, "' matches ", matches.size(), " snapshots; give more digits"));
  }
  return matches[0];
}

}  // namespace dds
