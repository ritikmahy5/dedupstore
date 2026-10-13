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
#include "dds/store/types.h"

namespace dds {

// Container file layout (ON_DISK_FORMAT.md section 5):
//   offset 0              header, 32 bytes
//   offset 32             blob payloads back to back, as stored (zstd frames or content, D107)
//   footer_offset         footer: u32 entry_count, then 50 bytes per entry (45 in version 1)
//   file end - 32         trailer, 32 bytes
inline constexpr size_t kContainerHeaderSize = 32;
inline constexpr size_t kContainerTrailerSize = 32;
inline constexpr size_t kFooterEntrySizeV1 = 45;
inline constexpr size_t kFooterEntrySize = 50;  // version 2 adds the codec and raw length (D107)
// Header + one 1-byte blob + a one-entry version 1 footer + trailer: the smallest container.
inline constexpr size_t kMinContainerFileSize =
    kContainerHeaderSize + 1 + 4 + kFooterEntrySizeV1 + kContainerTrailerSize;
// New containers are written as version 2; version 1 containers are still read (D107).
inline constexpr uint32_t kContainerFormatVersion = 2;
inline constexpr uint32_t kOldestContainerFormatVersion = 1;

// The footer entry size of a format version: 45 for 1, 50 for 2.
size_t FooterEntrySize(uint32_t version);
// Tree and recipe blobs (SPEC.md section 6).
inline constexpr uint32_t kMaxMetadataBlobBytes = 64u << 20;

// "0000002a.ctr": 8 lowercase hex digits of the id.
std::string ContainerFileName(uint32_t id);
// The id named by a container file name, or nullopt for any other name (including id 0, which is
// never allocated).
std::optional<uint32_t> ParseContainerFileName(std::string_view name);

// A container's validated metadata: what open and GC need from its footer.
struct ContainerMetadata {
  uint32_t id = 0;
  uint64_t file_size = 0;
  uint64_t payload_bytes = 0;
  std::vector<SealedEntry> entries;  // in blob-region order
};

std::array<std::byte, kContainerHeaderSize> EncodeContainerHeader(
    uint32_t id, const RepoId& repo_id, uint32_t version = kContainerFormatVersion);

// The footer and trailer for a container whose header is `header` and whose blobs, in order, are
// `entries` (offsets starting at 32, back to back), in the header's format version. Append the
// result after the payloads. A version 1 footer cannot record a codec, so every entry must then be
// stored as content.
std::vector<std::byte> EncodeContainerTail(std::span<const std::byte, kContainerHeaderSize> header,
                                           std::span<const SealedEntry> entries);

// Validates a container's header, footer, and trailer in the order of ON_DISK_FORMAT.md 5.5 and
// returns its entries. `name` (the file's path) starts every error message, which also gives the
// offset of the bad field. A version other than 1 under valid checksums is Incompatible; anything
// else wrong is Corruption. Payload bytes are not read: their CRCs are checked on every blob read.
//
// `header` is the file's first 32 bytes, `footer` the bytes from footer_offset up to the trailer,
// `trailer` the last 32 bytes. The trailer is parsed first (ParseContainerTrailer), so a reader
// knows how much footer to read.
struct ContainerTrailer {
  uint32_t footer_offset = 0;
  uint32_t footer_length = 0;
  uint32_t entry_count = 0;
  uint32_t footer_crc = 0;
  uint32_t header_crc = 0;
  uint64_t payload_bytes = 0;
};
Result<ContainerTrailer> ParseContainerTrailer(std::span<const std::byte> trailer,
                                               uint64_t file_size, std::string_view name);
Result<ContainerMetadata> DecodeContainerParts(std::span<const std::byte> header,
                                               std::span<const std::byte> footer,
                                               const ContainerTrailer& trailer, uint64_t file_size,
                                               std::string_view name, uint32_t expected_id,
                                               const RepoId& repo_id, uint32_t max_data_size);

// The same validation over a whole container file in memory (tests and the fuzz target).
Result<ContainerMetadata> DecodeContainerMetadata(std::span<const std::byte> file,
                                                  std::string_view name, uint32_t expected_id,
                                                  const RepoId& repo_id, uint32_t max_data_size);

}  // namespace dds
