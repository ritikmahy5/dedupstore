#include "dds/repo/index_checkpoint_format.h"

#include <algorithm>
#include <cstdint>

#include "dds/common/check.h"
#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "dds/store/container_format.h"

namespace dds {

namespace {

constexpr std::string_view kMagic = "DDSIDX01";

Status Corrupt(std::string_view name, uint64_t offset, std::string_view what) {
  return Status::Corruption(StrCat(name, ": offset ", offset, ": ", what));
}

// A covered container's sizes must satisfy the identities its own trailer does (ON_DISK_FORMAT.md
// 5.4), with the footer entry size of either container version: a container that could not exist
// is a damaged checkpoint.
bool PlausibleContainer(const ContainerInfo& info) {
  auto file_size = [&](size_t entry_size) {
    return kContainerHeaderSize + info.payload_bytes + 4 + uint64_t{entry_size} * info.entry_count +
           kContainerTrailerSize;
  };
  return info.entry_count >= 1 && info.payload_bytes >= info.entry_count &&
         (info.file_bytes == file_size(kFooterEntrySizeV1) ||
          info.file_bytes == file_size(kFooterEntrySize)) &&
         info.file_bytes <= UINT32_MAX;
}

// The table entry for `id`, or nullptr.
const CheckpointContainer* FindContainer(const CheckpointTables& tables, uint32_t id) {
  auto it =
      std::lower_bound(tables.containers.begin(), tables.containers.end(), id,
                       [](const CheckpointContainer& c, uint32_t value) { return c.id < value; });
  return it != tables.containers.end() && it->id == id ? &*it : nullptr;
}

// Checks one decoded entry of shard `shard` (ON_DISK_FORMAT.md 9.4). A stored length within the
// content limit is all that can be checked: a compressed blob is smaller than its content.
Status CheckEntry(const SealedEntry& e, uint8_t type_byte, uint8_t codec_byte, size_t shard,
                  const CheckpointTables& tables, uint64_t at, std::string_view name,
                  uint32_t max_data_size) {
  if (e.fp.ShardByte() != shard) return Corrupt(name, at, StrCat("entry is not in shard ", shard));
  if (!BlobTypeFromByte(type_byte)) {
    return Corrupt(name, at + 32, StrCat("invalid blob type ", type_byte));
  }
  if (!CodecFromByte(codec_byte)) return Corrupt(name, at + 33, "invalid codec");
  const CheckpointContainer* c = FindContainer(tables, e.loc.container_id);
  if (c == nullptr) {
    return Corrupt(name, at + 34,
                   StrCat("container ", e.loc.container_id, " is not in the container table"));
  }
  const uint32_t limit = e.loc.type == BlobType::kData ? max_data_size : kMaxMetadataBlobBytes;
  if (e.loc.length == 0 || e.loc.length > limit) {
    return Corrupt(name, at + 42, StrCat("blob length ", e.loc.length, " is outside 1..", limit));
  }
  if (e.loc.offset < kContainerHeaderSize ||
      uint64_t{e.loc.offset} + e.loc.length > kContainerHeaderSize + c->info.payload_bytes) {
    return Corrupt(name, at + 38, "blob range is outside its container's payloads");
  }
  return Status::Ok();
}

}  // namespace

void AppendCheckpointEntry(const SealedEntry& entry, std::vector<std::byte>& out) {
  Writer w;
  w.Bytes(entry.fp.AsBytes());
  w.U8(static_cast<uint8_t>(entry.loc.type));
  w.U8(static_cast<uint8_t>(entry.loc.codec));
  w.U32(entry.loc.container_id);
  w.U32(entry.loc.offset);
  w.U32(entry.loc.length);
  w.U32(entry.loc.crc32c);
  out.insert(out.end(), w.buffer().begin(), w.buffer().end());
}

std::vector<std::byte> EncodeCheckpointTail(const CheckpointTables& tables, const RepoId& repo_id) {
  Writer w;
  for (const CheckpointContainer& c : tables.containers) {
    w.U32(c.id);
    w.U64(c.info.payload_bytes);
    w.U64(c.info.file_bytes);
    w.U32(c.info.entry_count);
  }
  for (const CheckpointShard& s : tables.shards) {
    w.U32(s.entry_count);
    w.U32(s.crc32c);
  }
  const uint32_t tables_crc = Crc32c(w.buffer());
  Writer trailer;
  trailer.Bytes(AsBytes(kMagic));
  trailer.U32(kCheckpointFormatVersion);
  for (uint8_t b : repo_id) trailer.U8(b);
  trailer.U32(tables.next_container_id);
  trailer.U32(static_cast<uint32_t>(tables.containers.size()));
  trailer.U64(tables.entry_count);
  trailer.U32(tables_crc);
  trailer.U32(Crc32c(trailer.buffer()));
  std::vector<std::byte> out = w.Take();
  out.insert(out.end(), trailer.buffer().begin(), trailer.buffer().end());
  return out;
}

Result<CheckpointTrailer> ParseCheckpointTrailer(std::span<const std::byte> trailer,
                                                 uint64_t file_size, std::string_view name,
                                                 const RepoId& repo_id) {
  if (file_size < kCheckpointShardTableSize + kCheckpointTrailerSize) {
    return Corrupt(name, 0, StrCat("file is ", file_size, " bytes, smaller than any checkpoint"));
  }
  DDS_CHECK(trailer.size() == kCheckpointTrailerSize);
  const uint64_t at = file_size - kCheckpointTrailerSize;
  // The trailer's own CRC first, so a damaged version field reads as corruption, not a new version.
  if (Crc32c(trailer.first(48)) != LoadU32(trailer.data() + 48)) {
    return Corrupt(name, at + 48, "trailer CRC");
  }
  if (AsChars(trailer.first(8)) != kMagic) return Corrupt(name, at, "bad trailer magic");
  const uint32_t version = LoadU32(trailer.data() + 8);
  if (version != kCheckpointFormatVersion) {
    return Status::Incompatible(StrCat(name, ": index checkpoint version ", version,
                                       " is not supported (this build reads version 2)"));
  }
  if (!std::equal(repo_id.begin(), repo_id.end(), trailer.begin() + 12,
                  [](uint8_t a, std::byte b) { return a == static_cast<uint8_t>(b); })) {
    return Corrupt(name, at + 12, "repo_id differs from the repository's config");
  }
  CheckpointTrailer t;
  t.next_container_id = LoadU32(trailer.data() + 28);
  t.container_count = LoadU32(trailer.data() + 32);
  t.entry_count = LoadU64(trailer.data() + 36);
  t.tables_crc = LoadU32(trailer.data() + 44);
  t.tables_size =
      uint64_t{kCheckpointContainerSize} * t.container_count + kCheckpointShardTableSize;
  // Bounded before multiplying, so the size identity below cannot overflow.
  if (t.entry_count > file_size / kCheckpointEntrySize) {
    return Corrupt(name, at + 36, "entry_count does not fit in the file");
  }
  t.tables_offset = kCheckpointEntrySize * t.entry_count;
  if (t.tables_offset + t.tables_size + kCheckpointTrailerSize != file_size) {
    return Corrupt(name, at + 32, "entries + tables + trailer do not add up to the file size");
  }
  if (t.next_container_id == 0) return Corrupt(name, at + 28, "next_container_id is 0");
  return t;
}

Result<CheckpointTables> DecodeCheckpointTables(std::span<const std::byte> bytes,
                                                const CheckpointTrailer& trailer,
                                                std::string_view name) {
  DDS_CHECK(bytes.size() == trailer.tables_size);
  const uint64_t at = trailer.tables_offset;
  if (Crc32c(bytes) != trailer.tables_crc) return Corrupt(name, at, "tables CRC");
  CheckpointTables tables;
  tables.next_container_id = trailer.next_container_id;
  tables.entry_count = trailer.entry_count;
  tables.containers.resize(trailer.container_count);
  Reader r(bytes);
  uint32_t previous = 0;
  for (uint32_t i = 0; i < trailer.container_count; ++i) {
    CheckpointContainer& c = tables.containers[i];
    // The size identity fixed tables_size, so these reads cannot run out of bytes.
    DDS_CHECK(r.U32(&c.id) && r.U64(&c.info.payload_bytes) && r.U64(&c.info.file_bytes) &&
              r.U32(&c.info.entry_count));
    const uint64_t pos = at + uint64_t{kCheckpointContainerSize} * i;
    if (c.id <= previous || c.id >= tables.next_container_id) {
      return Corrupt(name, pos,
                     StrCat("container id ", c.id, " is out of order or not below ",
                            tables.next_container_id));
    }
    if (!PlausibleContainer(c.info)) {
      return Corrupt(name, pos + 4, StrCat("container ", c.id, "'s sizes are inconsistent"));
    }
    previous = c.id;
  }
  uint64_t offset = 0;
  for (CheckpointShard& s : tables.shards) {
    DDS_CHECK(r.U32(&s.entry_count) && r.U32(&s.crc32c));
    s.offset = offset;
    offset += uint64_t{kCheckpointEntrySize} * s.entry_count;
  }
  if (offset != trailer.tables_offset) {
    return Corrupt(name, at + uint64_t{kCheckpointContainerSize} * trailer.container_count,
                   "shard entry counts do not add up to entry_count");
  }
  return tables;
}

Status DecodeCheckpointShard(std::span<const std::byte> bytes, size_t shard,
                             const CheckpointTables& tables, std::string_view name,
                             uint32_t max_data_size, std::vector<SealedEntry>* out) {
  const CheckpointShard& s = tables.shards[shard];
  DDS_CHECK(bytes.size() == uint64_t{kCheckpointEntrySize} * s.entry_count);
  if (Crc32c(bytes) != s.crc32c) return Corrupt(name, s.offset, StrCat("shard ", shard, " CRC"));
  Reader r(bytes);
  for (uint32_t i = 0; i < s.entry_count; ++i) {
    const uint64_t at = s.offset + uint64_t{kCheckpointEntrySize} * i;
    std::span<const std::byte> fp;
    uint8_t type = 0;
    uint8_t codec = 0;
    SealedEntry e;
    DDS_CHECK(r.Bytes(32, &fp) && r.U8(&type) && r.U8(&codec) && r.U32(&e.loc.container_id) &&
              r.U32(&e.loc.offset) && r.U32(&e.loc.length) && r.U32(&e.loc.crc32c));
    e.fp = Fingerprint::FromBytes(fp.first<32>());
    e.loc.type = BlobTypeFromByte(type).value_or(BlobType::kData);
    e.loc.codec = CodecFromByte(codec).value_or(Codec::kNone);
    DDS_TRY(CheckEntry(e, type, codec, shard, tables, at, name, max_data_size));
    // Strictly increasing: one entry per fingerprint, and one canonical encoding per checkpoint.
    if (i > 0 && !(out->back().fp < e.fp)) {
      return Corrupt(name, at, "entries are not in increasing fingerprint order");
    }
    out->push_back(e);
  }
  return Status::Ok();
}

Result<DecodedCheckpoint> DecodeIndexCheckpoint(std::span<const std::byte> file,
                                                std::string_view name, const RepoId& repo_id,
                                                uint32_t max_data_size) {
  if (file.size() < kCheckpointTrailerSize) {
    return Corrupt(name, 0, StrCat("file is ", file.size(), " bytes, smaller than any checkpoint"));
  }
  auto trailer =
      ParseCheckpointTrailer(file.last(kCheckpointTrailerSize), file.size(), name, repo_id);
  DDS_TRY(trailer.status());
  // ParseCheckpointTrailer checked the sizes against file.size(), so these slices are in bounds.
  auto tables = DecodeCheckpointTables(file.subspan(trailer->tables_offset, trailer->tables_size),
                                       *trailer, name);
  DDS_TRY(tables.status());
  DecodedCheckpoint decoded{std::move(tables).value(), {}};
  for (size_t shard = 0; shard < kCheckpointShards; ++shard) {
    const CheckpointShard& s = decoded.tables.shards[shard];
    DDS_TRY(DecodeCheckpointShard(
        file.subspan(s.offset, uint64_t{kCheckpointEntrySize} * s.entry_count), shard,
        decoded.tables, name, max_data_size, &decoded.entries));
  }
  return decoded;
}

}  // namespace dds
