#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dds/common/result.h"
#include "dds/hash/fingerprint.h"

namespace dds {

inline constexpr uint32_t kMaxPiecesPerFile = 65536;  // D14
inline constexpr uint32_t kTreeFormatVersion = 1;

enum class EntryKind : uint8_t { kFile = 1, kDirectory = 2, kSymlink = 3 };

// One entry of a directory listing (ON_DISK_FORMAT.md section 6). Which fields matter depends on
// `kind`: a file has `size` and `pieces` (its recipe blobs in order), a directory `subtree`, a
// symlink `target`.
struct TreeEntry {
  std::string name;
  EntryKind kind = EntryKind::kFile;
  uint32_t mode = 0;        // permission bits only (mode & 07777 == mode)
  int64_t mtime_sec = 0;    // signed: times before 1970 exist
  uint32_t mtime_nsec = 0;  // < 1,000,000,000
  uint64_t size = 0;
  std::vector<Fingerprint> pieces;
  Fingerprint subtree;
  std::string target;
};

// A directory: entries strictly ascending by name compared as unsigned bytes. The order makes the
// encoding, and so the tree's fingerprint, independent of how the directory was read (G11).
struct Tree {
  std::vector<TreeEntry> entries;
};

// Encodes a tree blob. InvalidArgument if any entry breaks section 6's rules or the blob would
// exceed the 64 MiB metadata limit (SPEC.md section 6): the backup then fails, naming the
// directory.
Result<std::vector<std::byte>> EncodeTree(const Tree& tree);

// Decodes and validates a tree blob: Corruption for anything section 6 does not allow (names,
// order, kinds, modes, nanoseconds, piece counts, targets, trailing bytes), Incompatible for an
// unknown version.
Result<Tree> DecodeTree(std::span<const std::byte> bytes);

}  // namespace dds
