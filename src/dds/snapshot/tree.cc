#include "dds/snapshot/tree.h"

#include <optional>
#include <string>

#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "dds/snapshot/names.h"
#include "dds/store/container_format.h"

namespace dds {

namespace {

// Smallest possible entry: a 1-byte name (2 + 1), kind, mode, mtime (8 + 4), and the smallest
// kind-specific part (a symlink's 1-byte target, 3 bytes). Bounds entry_count before reserving.
constexpr size_t kMinEntryBytes = 3 + 1 + 4 + 12 + 3;

// The rules of ON_DISK_FORMAT.md section 6 for one entry, shared by encoder and decoder.
std::optional<std::string> EntryProblem(const TreeEntry& e) {
  if (!IsValidName(e.name)) return "invalid name";
  if ((e.mode & ~07777u) != 0) return StrCat("mode ", e.mode, " has bits beyond the permissions");
  if (e.mtime_nsec > 999999999) return StrCat("mtime nanoseconds ", e.mtime_nsec, " out of range");
  switch (e.kind) {
    case EntryKind::kFile:
      if (e.pieces.size() > kMaxPiecesPerFile) return "more than 65536 recipe pieces";
      // An empty file has no pieces, and a non-empty file has at least one.
      if ((e.size == 0) != e.pieces.empty()) return "size and piece count disagree";
      return std::nullopt;
    case EntryKind::kDirectory:
      return std::nullopt;
    case EntryKind::kSymlink:
      if (!IsValidSymlinkTarget(e.target)) return "invalid symlink target";
      return std::nullopt;
  }
  return "invalid kind";
}

Status Corrupt(size_t index, std::string_view what) {
  return Status::Corruption(StrCat("tree entry ", index, ": ", what));
}

// Reads one entry's common fields and its kind-specific part.
Status ReadEntry(Reader& r, size_t index, TreeEntry* e) {
  uint8_t kind = 0;
  if (!r.Str16(&e->name) || !r.U8(&kind) || !r.U32(&e->mode) || !r.I64(&e->mtime_sec) ||
      !r.U32(&e->mtime_nsec)) {
    return Corrupt(index, "truncated");
  }
  if (kind < 1 || kind > 3) return Corrupt(index, StrCat("invalid kind ", kind));
  e->kind = static_cast<EntryKind>(kind);
  if (e->kind == EntryKind::kFile) {
    uint32_t count = 0;
    if (!r.U64(&e->size) || !r.U32(&count)) return Corrupt(index, "truncated");
    if (count > kMaxPiecesPerFile || count > r.remaining() / 32) {
      return Corrupt(index, StrCat("piece count ", count, " is impossible"));
    }
    e->pieces.resize(count);
    for (Fingerprint& fp : e->pieces) {
      std::span<const std::byte> raw;
      DDS_CHECK(r.Bytes(32, &raw));  // count was checked against remaining()
      fp = Fingerprint::FromBytes(raw.first<32>());
    }
  } else if (e->kind == EntryKind::kDirectory) {
    std::span<const std::byte> raw;
    if (!r.Bytes(32, &raw)) return Corrupt(index, "truncated");
    e->subtree = Fingerprint::FromBytes(raw.first<32>());
  } else {
    if (!r.Str16(&e->target)) return Corrupt(index, "truncated");
  }
  if (auto problem = EntryProblem(*e)) return Corrupt(index, *problem);
  return Status::Ok();
}

}  // namespace

Result<std::vector<std::byte>> EncodeTree(const Tree& tree) {
  Writer w;
  w.U8(kTreeFormatVersion);
  w.U32(static_cast<uint32_t>(tree.entries.size()));
  for (size_t i = 0; i < tree.entries.size(); ++i) {
    const TreeEntry& e = tree.entries[i];
    if (auto problem = EntryProblem(e)) {
      return Status::InvalidArgument(StrCat("entry ", i, ": ", *problem));
    }
    if (i > 0 && !(tree.entries[i - 1].name < e.name)) {
      return Status::InvalidArgument(StrCat("entry ", i, ": names not strictly ascending"));
    }
    w.Str16(e.name);
    w.U8(static_cast<uint8_t>(e.kind));
    w.U32(e.mode);
    w.I64(e.mtime_sec);
    w.U32(e.mtime_nsec);
    if (e.kind == EntryKind::kFile) {
      w.U64(e.size);
      w.U32(static_cast<uint32_t>(e.pieces.size()));
      for (const Fingerprint& fp : e.pieces) w.Bytes(fp.AsBytes());
    } else if (e.kind == EntryKind::kDirectory) {
      w.Bytes(e.subtree.AsBytes());
    } else {
      w.Str16(e.target);
    }
  }
  if (w.size() > kMaxMetadataBlobBytes) {
    return Status::InvalidArgument(
        StrCat("directory listing would be ", w.size(), " bytes, more than the 64 MiB limit"));
  }
  return w.Take();
}

Result<Tree> DecodeTree(std::span<const std::byte> bytes) {
  Reader r(bytes);
  uint8_t version = 0;
  uint32_t count = 0;
  if (!r.U8(&version)) return Status::Corruption("tree: empty blob");
  if (version != kTreeFormatVersion) {
    return Status::Incompatible(StrCat("tree format version ", version, " is not supported"));
  }
  if (!r.U32(&count)) return Status::Corruption("tree: truncated header");
  if (count > r.remaining() / kMinEntryBytes) {
    return Status::Corruption(StrCat("tree: entry count ", count, " is impossible"));
  }
  Tree tree;
  tree.entries.resize(count);
  for (size_t i = 0; i < count; ++i) {
    DDS_TRY(ReadEntry(r, i, &tree.entries[i]));
    // std::string's < compares as unsigned bytes (char_traits<char>), the order section 6 needs.
    if (i > 0 && !(tree.entries[i - 1].name < tree.entries[i].name)) {
      return Corrupt(i, "names not strictly ascending");
    }
  }
  if (!r.empty()) return Status::Corruption("tree: unexpected bytes after the last entry");
  return tree;
}

}  // namespace dds
