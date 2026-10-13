#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "dds/common/result.h"
#include "dds/store/types.h"

namespace dds {

// Index checkpoint file layout (ON_DISK_FORMAT.md section 9, D104):
//   offset 0          entries, 50 bytes each, grouped by index shard 0..255 and sorted by
//                     fingerprint within a shard
//   50 x entries      container table, 24 bytes per covered container, ids increasing
//   then              shard table, 256 x 8 bytes: entry count and CRC32C of that shard's entries
//   file end - 52     trailer, 52 bytes
// Like a container, the tables and trailer come last, so the writer only ever appends.
inline constexpr size_t kCheckpointEntrySize = 50;
inline constexpr size_t kCheckpointContainerSize = 24;
inline constexpr size_t kCheckpointShards = 256;  // ChunkIndex::kShardCount
inline constexpr size_t kCheckpointShardTableSize = kCheckpointShards * 8;
inline constexpr size_t kCheckpointTrailerSize = 52;
// Version 2 adds each entry's codec (D107). A version 1 checkpoint is Incompatible, and open
// removes it and reads the footers: it is derived, so nothing is lost.
inline constexpr uint32_t kCheckpointFormatVersion = 2;

// A container the checkpoint covers: its entries are in the file, and its sizes are what
// ContainerSet would have read from its footer.
struct CheckpointContainer {
  uint32_t id = 0;
  ContainerInfo info;
};

struct CheckpointShard {
  uint32_t entry_count = 0;
  uint32_t crc32c = 0;  // of the shard's entry bytes
  uint64_t offset = 0;  // where its entries start; derived from the counts, not stored
};

// Everything in the file but the entries.
struct CheckpointTables {
  uint32_t next_container_id = 0;  // the writer's next id: open never allocates below it
  uint64_t entry_count = 0;
  std::vector<CheckpointContainer> containers;  // ids strictly increasing
  std::array<CheckpointShard, kCheckpointShards> shards{};
};

struct CheckpointTrailer {
  uint32_t next_container_id = 0;
  uint32_t container_count = 0;
  uint64_t entry_count = 0;
  uint32_t tables_crc = 0;
  uint64_t tables_offset = 0;  // derived: 50 x entry_count
  uint64_t tables_size = 0;    // derived: 24 x container_count + 2048
};

// Appends one entry's 50 bytes. The writer appends each shard's entries in fingerprint order.
void AppendCheckpointEntry(const SealedEntry& entry, std::vector<std::byte>& out);

// The container table, shard table, and trailer for `tables` (shard CRCs filled in), to append
// after the entries.
std::vector<std::byte> EncodeCheckpointTail(const CheckpointTables& tables, const RepoId& repo_id);

// Validates the trailer (its CRC, magic, version, repo_id, and the size identity) against the file
// size. A version other than 1 under a valid trailer CRC is Incompatible; anything else wrong is
// Corruption naming `name` and the offset.
Result<CheckpointTrailer> ParseCheckpointTrailer(std::span<const std::byte> trailer,
                                                 uint64_t file_size, std::string_view name,
                                                 const RepoId& repo_id);

// Validates the tables (`bytes` = the tables_size bytes at tables_offset) and derives each
// shard's offset.
Result<CheckpointTables> DecodeCheckpointTables(std::span<const std::byte> bytes,
                                                const CheckpointTrailer& trailer,
                                                std::string_view name);

// Validates shard `shard`'s entries (`bytes` = exactly its 50 x entry_count bytes) and appends them
// to `out`: the CRC, then for each entry its shard, order, type, codec, container, and byte range.
Status DecodeCheckpointShard(std::span<const std::byte> bytes, size_t shard,
                             const CheckpointTables& tables, std::string_view name,
                             uint32_t max_data_size, std::vector<SealedEntry>* out);

// A whole checkpoint file in memory, for tests and the fuzz target.
struct DecodedCheckpoint {
  CheckpointTables tables;
  std::vector<SealedEntry> entries;  // shard by shard, as in the file
};
Result<DecodedCheckpoint> DecodeIndexCheckpoint(std::span<const std::byte> file,
                                                std::string_view name, const RepoId& repo_id,
                                                uint32_t max_data_size);

}  // namespace dds
