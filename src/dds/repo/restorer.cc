#include "dds/repo/restorer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "dds/common/strings.h"
#include "dds/repo/blob_reader.h"
#include "dds/repo/repository.h"

namespace dds {

namespace {

EntryMeta MetaOf(const TreeEntry& e) { return {e.mode, e.mtime_sec, e.mtime_nsec}; }

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

class Restorer {
 public:
  Restorer(RepoInternals& repo, const SnapshotId& id, RestoreSink& sink, bool verify)
      : repo_(repo), id_(id), sink_(sink), reader_(repo, verify) {}

  Status RestoreDir(const Fingerprint& tree_fp, RestoreSink::DirHandle dir,
                    const std::string& path);
  RestoreResult result() const { return result_; }

 private:
  Status RestoreEntry(const Tree& tree, size_t index, RestoreSink::DirHandle dir,
                      const std::string& parent);
  Status RestoreFile(const TreeEntry& e, RestoreSink::DirHandle dir, const std::string& path);
  // A blob the snapshot needs is gone: Aborted if the snapshot itself was removed meanwhile (a
  // concurrent `rm` in a serving process), otherwise Corruption (ARCHITECTURE.md 4.2).
  Status Explain(const Status& s, const std::string& path);
  // An AlreadyExists from the sink, naming the sibling it collides with if there is one.
  Status Collision(const Tree& tree, size_t index, const std::string& path, const Status& s);

  RepoInternals& repo_;
  const SnapshotId id_;
  RestoreSink& sink_;
  BlobReader reader_;
  RestoreResult result_;
  // Trees on the current path. With --no-verify, a damaged index can claim a tree that contains
  // itself; following it would recurse forever (see DagWalker).
  std::vector<Fingerprint> ancestors_;
};

Status Restorer::Explain(const Status& s, const std::string& path) {
  const std::string where = path.empty() ? "the snapshot root" : path;
  if (s.code() != StatusCode::kNotFound) return s.WithContext(where);
  auto exists = repo_.snapshots.Exists(id_);
  if (exists.ok() && !*exists) {
    return Status::Aborted(
        StrCat("snapshot ", SnapshotIdToHex(id_), " was removed while restoring"));
  }
  return Status::Corruption(StrCat(where, ": ", s.message()));
}

Status Restorer::Collision(const Tree& tree, size_t index, const std::string& path,
                           const Status& s) {
  const std::string name = Lower(tree.entries[index].name);
  for (size_t i = 0; i < index; ++i) {
    if (Lower(tree.entries[i].name) == name) {
      return Status::AlreadyExists(StrCat(path, ": collides with '", tree.entries[i].name,
                                          "', which the destination cannot hold separately (a "
                                          "case-insensitive volume?)"));
    }
  }
  return s;
}

Status Restorer::RestoreDir(const Fingerprint& tree_fp, RestoreSink::DirHandle dir,
                            const std::string& path) {
  if (std::find(ancestors_.begin(), ancestors_.end(), tree_fp) != ancestors_.end()) {
    return Status::Corruption(StrCat(path, ": tree ", tree_fp.ToHex(), " contains itself"));
  }
  auto tree = [&] {
    ReadGuard guard(repo_.epochs);  // one tree blob is one guarded unit (6.3)
    return reader_.ReadTree(tree_fp);
  }();
  if (!tree.ok()) return Explain(tree.status(), path);
  ancestors_.push_back(tree_fp);
  for (size_t i = 0; i < tree->entries.size(); ++i) DDS_TRY(RestoreEntry(*tree, i, dir, path));
  ancestors_.pop_back();
  return Status::Ok();
}

Status Restorer::RestoreEntry(const Tree& tree, size_t index, RestoreSink::DirHandle dir,
                              const std::string& parent) {
  const TreeEntry& e = tree.entries[index];
  const std::string path = ChildPath(parent, e.name);
  Status status;
  switch (e.kind) {
    case EntryKind::kFile:
      status = RestoreFile(e, dir, path);
      ++result_.files;
      break;
    case EntryKind::kSymlink:
      status = sink_.MakeSymlink(dir, e.name, e.target, MetaOf(e));
      ++result_.symlinks;
      break;
    case EntryKind::kDirectory: {
      auto child = sink_.MakeDir(dir, e.name);
      if (!child.ok()) {
        status = child.status();
        break;
      }
      DDS_TRY(RestoreDir(e.subtree, *child, path));
      // After all children, so a read-only directory does not block its own restore.
      status = sink_.FinishDir(*child, MetaOf(e));
      ++result_.directories;
      break;
    }
  }
  if (status.code() == StatusCode::kAlreadyExists) return Collision(tree, index, path, status);
  return status;
}

Status Restorer::RestoreFile(const TreeEntry& e, RestoreSink::DirHandle dir,
                             const std::string& path) {
  auto file = sink_.CreateFile(dir, e.name);
  DDS_TRY(file.status());
  ReadGuard guard(repo_.epochs);  // one file is one guarded unit (6.3, D21)
  uint64_t written = 0;
  for (const Fingerprint& piece_fp : e.pieces) {
    auto piece = reader_.ReadRecipe(piece_fp);
    if (!piece.ok()) return Explain(piece.status(), path);
    for (const ChunkRef& ref : piece->refs) {
      auto chunk = reader_.Read(ref.fp, BlobType::kData);
      if (!chunk.ok()) return Explain(chunk.status(), path);
      if (chunk->size() != ref.length) {
        return Status::Corruption(StrCat(path, ": chunk ", ref.fp.ToHex(), " is ", chunk->size(),
                                         " bytes, the recipe says ", ref.length));
      }
      DDS_TRY((*file)->Write(*chunk));
      written += chunk->size();
    }
  }
  // ON_DISK_FORMAT.md section 6: the chunk lengths must add up to the recorded size.
  if (written != e.size) {
    return Status::Corruption(
        StrCat(path, ": restored ", written, " bytes, the tree says ", e.size));
  }
  result_.bytes += written;
  return (*file)->Finish(MetaOf(e));
}

}  // namespace

Result<RestoreResult> RunRestore(RepoInternals& repo, const SnapshotId& id, RestoreSink& sink,
                                 const RestoreOptions& options) {
  auto manifest = repo.snapshots.Read(id);
  DDS_TRY(manifest.status());
  auto root = sink.OpenRoot();
  DDS_TRY(root.status());
  Restorer restorer(repo, id, sink, options.verify);
  DDS_TRY(restorer.RestoreDir(manifest->root, *root, ""));
  // The destination's own mode and times are not part of a snapshot (D68).
  DDS_TRY(sink.FinishDir(*root, std::nullopt));
  return restorer.result();
}

}  // namespace dds
