#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "dds/hash/fingerprint.h"
#include "dds/store/compression.h"

namespace dds {

// 16 random bytes chosen at init (ON_DISK_FORMAT.md section 3). Every container header carries it,
// so a container copied in from another repository is rejected on open.
using RepoId = std::array<uint8_t, 16>;

// Where one blob's bytes are: a byte range of one container file, with the CRC32C that every read
// checks (D26), the blob's type, and how its content is stored there (D107).
struct Location {
  uint32_t container_id = 0;
  uint32_t offset = 0;  // from the start of the container file
  uint32_t length = 0;  // bytes as stored, >= 1
  uint32_t crc32c = 0;  // of the bytes as stored
  BlobType type = BlobType::kData;
  Codec codec = Codec::kNone;

  friend bool operator==(const Location&, const Location&) = default;
};

// A blob in a container: its fingerprint and location. Produced by sealing (ContainerWriter) and
// by reading a container's footer (open).
struct SealedEntry {
  Fingerprint fp;
  Location loc;
  // The content's length (loc.length for Codec::kNone). Known where entries come from a writer or a
  // footer; 0 where they come from the index or an index checkpoint, which keep stored lengths
  // only.
  uint32_t raw_length = 0;
};

// What sealing a container produced.
struct SealedContainer {
  uint32_t id = 0;
  uint64_t payload_bytes = 0;
  uint64_t file_bytes = 0;
  std::vector<SealedEntry> entries;
};

// Sizes of a published container, kept in memory by ContainerSet (GC's planner and `stats` use
// them).
struct ContainerInfo {
  uint64_t payload_bytes = 0;
  uint64_t file_bytes = 0;
  uint32_t entry_count = 0;
};

}  // namespace dds
