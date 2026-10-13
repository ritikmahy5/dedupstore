#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dds/common/result.h"
#include "dds/hash/fingerprint.h"
#include "dds/store/types.h"

namespace dds {

// Version 2 adds started_at_ns (D106). Version 1 manifests still decode, and re-encode to the same
// bytes, so their snapshot ids never change.
inline constexpr uint32_t kManifestFormatVersion = 2;
inline constexpr uint32_t kOldestManifestFormatVersion = 1;

// A snapshot's id: the first 16 bytes of BLAKE3 over its manifest file (D15). Content-derived, so
// a snapshot has the same id in every repository it is replicated to.
using SnapshotId = std::array<uint8_t, 16>;

SnapshotId SnapshotIdOf(std::span<const std::byte> manifest_bytes);
std::string SnapshotIdToHex(const SnapshotId& id);
std::optional<SnapshotId> SnapshotIdFromHex(std::string_view hex);

// Limits on the manifest's strings (ON_DISK_FORMAT.md section 8).
inline constexpr size_t kMaxHostBytes = 255;
inline constexpr size_t kMaxSourcePathBytes = 4095;
inline constexpr size_t kMaxTagBytes = 1024;

// A snapshot's commit record (ON_DISK_FORMAT.md section 8): the root tree and summary counts.
struct Manifest {
  uint32_t format_version =
      kManifestFormatVersion;  // the version it was decoded from or encodes to
  RepoId origin_repo_id{};     // the repository that created the snapshot; kept by replication
  int64_t created_at_ns = 0;   // wall clock when the manifest was built, at the end of the backup
  // Version 2: wall clock when the backup started listing (D106); 0 in version 1. A later backup
  // uses it to decide which of this snapshot's files it may skip as unchanged.
  int64_t started_at_ns = 0;
  Fingerprint root;
  uint64_t logical_bytes = 0;
  uint64_t file_count = 0;
  uint64_t directory_count = 0;
  uint64_t symlink_count = 0;
  std::string host;         // <= kMaxHostBytes
  std::string source_path;  // <= kMaxSourcePathBytes
  std::string tag;          // <= kMaxTagBytes
};

// InvalidArgument if a string field exceeds its limit, the version is unknown, or a version 1
// manifest has a start time.
Status ValidateManifest(const Manifest& manifest);

// The manifest file's bytes, ending with the CRC32C of everything before it. `manifest` must be
// valid.
std::vector<std::byte> EncodeManifest(const Manifest& manifest);

// Corruption for a bad magic, CRC, string length, or trailing bytes; Incompatible for an unknown
// version under a valid CRC.
Result<Manifest> DecodeManifest(std::span<const std::byte> bytes);

}  // namespace dds
