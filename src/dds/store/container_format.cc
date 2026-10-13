#include "dds/store/container_format.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"

namespace dds {

namespace {

constexpr std::string_view kHeaderMagic = "DDSCTR01";
constexpr std::string_view kTrailerMagic = "DDSE";

Status Corrupt(std::string_view name, uint64_t offset, std::string_view what) {
  return Status::Corruption(StrCat(name, ": offset ", offset, ": ", what));
}

Status CheckMinimumSize(uint64_t file_size, std::string_view name) {
  if (file_size < kMinContainerFileSize) {
    return Corrupt(name, 0,
                   StrCat("file is ", file_size, " bytes, smaller than any container (",
                          kMinContainerFileSize, ")"));
  }
  return Status::Ok();
}

// One footer entry as read, before its checks.
struct RawEntry {
  SealedEntry entry;
  uint8_t type = 0;
  uint8_t codec = 0;
};

// Reads one entry of a footer of format `version`. The caller checked the footer's size.
RawEntry ReadEntry(Reader& r, uint32_t version, uint32_t container_id) {
  RawEntry raw;
  SealedEntry& e = raw.entry;
  std::span<const std::byte> fp;
  DDS_CHECK(r.Bytes(32, &fp) && r.U8(&raw.type));
  if (version >= 2) {
    DDS_CHECK(r.U8(&raw.codec) && r.U32(&e.loc.offset) && r.U32(&e.loc.length) &&
              r.U32(&e.raw_length) && r.U32(&e.loc.crc32c));
  } else {
    DDS_CHECK(r.U32(&e.loc.offset) && r.U32(&e.loc.length) && r.U32(&e.loc.crc32c));
    e.raw_length = e.loc.length;  // version 1 stores every blob as its content
  }
  e.fp = Fingerprint::FromBytes(fp.first<32>());
  e.loc.container_id = container_id;
  e.loc.type = BlobTypeFromByte(raw.type).value_or(BlobType::kData);
  e.loc.codec = CodecFromByte(raw.codec).value_or(Codec::kNone);
  return raw;
}

// Step 3 of ON_DISK_FORMAT.md 5.5 for one entry: valid type and codec, contiguous offset, content
// length in limits, and a stored length that fits the codec. `entry_pos` is where it starts.
Status CheckEntry(const RawEntry& raw, uint32_t version, uint64_t expected_offset,
                  uint64_t entry_pos, std::string_view name, uint32_t max_data_size) {
  const SealedEntry& e = raw.entry;
  const uint64_t field = version >= 2 ? 1 : 0;  // version 2 has the codec byte after the type
  if (!BlobTypeFromByte(raw.type)) {
    return Corrupt(name, entry_pos + 32, StrCat("invalid blob type ", raw.type));
  }
  if (!CodecFromByte(raw.codec)) return Corrupt(name, entry_pos + 33, "invalid codec");
  if (e.loc.offset != expected_offset) {
    return Corrupt(name, entry_pos + 33 + field,
                   StrCat("entry offset ", e.loc.offset, " should be ", expected_offset));
  }
  const uint32_t limit = e.loc.type == BlobType::kData ? max_data_size : kMaxMetadataBlobBytes;
  if (e.raw_length == 0 || e.raw_length > limit) {
    return Corrupt(
        name, entry_pos + 37 + 2 * field,
        StrCat(BlobTypeName(e.loc.type), " blob length ", e.raw_length, " is outside 1..", limit));
  }
  // Content is stored as is; a zstd frame is stored only when smaller than its content (D107).
  const bool stored_ok = e.loc.codec == Codec::kNone
                             ? e.loc.length == e.raw_length
                             : e.loc.length >= 1 && e.loc.length < e.raw_length;
  if (!stored_ok) {
    return Corrupt(name, entry_pos + 37 + field,
                   StrCat("stored length ", e.loc.length, " does not fit ", CodecName(e.loc.codec),
                          " content of ", e.raw_length, " bytes"));
  }
  return Status::Ok();
}

}  // namespace

size_t FooterEntrySize(uint32_t version) {
  return version >= 2 ? kFooterEntrySize : kFooterEntrySizeV1;
}

std::string ContainerFileName(uint32_t id) { return HexU32(id) + ".ctr"; }

std::optional<uint32_t> ParseContainerFileName(std::string_view name) {
  if (name.size() != 12 || !name.ends_with(".ctr")) return std::nullopt;
  uint32_t id = 0;
  if (!ParseHexU32(name.substr(0, 8), &id) || id == 0) return std::nullopt;
  return id;
}

std::array<std::byte, kContainerHeaderSize> EncodeContainerHeader(uint32_t id,
                                                                  const RepoId& repo_id,
                                                                  uint32_t version) {
  DDS_CHECK(version >= kOldestContainerFormatVersion && version <= kContainerFormatVersion);
  Writer w;
  w.Bytes(AsBytes(kHeaderMagic));
  w.U32(version);
  w.U32(id);
  w.Bytes(std::as_bytes(std::span(repo_id)));
  std::array<std::byte, kContainerHeaderSize> out{};
  DDS_CHECK(w.size() == out.size());
  std::copy(w.buffer().begin(), w.buffer().end(), out.begin());
  return out;
}

std::vector<std::byte> EncodeContainerTail(std::span<const std::byte, kContainerHeaderSize> header,
                                           std::span<const SealedEntry> entries) {
  DDS_CHECK(!entries.empty());  // an empty container is never sealed (ON_DISK_FORMAT.md 5.3)
  const uint32_t version = LoadU32(header.data() + 8);
  Writer footer;
  footer.U32(static_cast<uint32_t>(entries.size()));
  for (const SealedEntry& e : entries) {
    footer.Bytes(e.fp.AsBytes());
    footer.U8(static_cast<uint8_t>(e.loc.type));
    if (version >= 2) footer.U8(static_cast<uint8_t>(e.loc.codec));
    DDS_CHECK(version >= 2 || e.loc.codec == Codec::kNone);
    footer.U32(e.loc.offset);
    footer.U32(e.loc.length);
    if (version >= 2) footer.U32(e.raw_length);
    footer.U32(e.loc.crc32c);
  }
  const uint64_t footer_offset = uint64_t{entries.back().loc.offset} + entries.back().loc.length;
  DDS_CHECK(footer_offset <= std::numeric_limits<uint32_t>::max());  // offsets are u32 (5.4)
  Writer trailer;
  trailer.U32(static_cast<uint32_t>(footer_offset));
  trailer.U32(static_cast<uint32_t>(footer.size()));
  trailer.U32(static_cast<uint32_t>(entries.size()));
  trailer.U32(Crc32c(footer.buffer()));
  trailer.U32(Crc32c(header));
  trailer.U64(footer_offset - kContainerHeaderSize);
  trailer.Bytes(AsBytes(kTrailerMagic));
  std::vector<std::byte> out = footer.Take();
  out.insert(out.end(), trailer.buffer().begin(), trailer.buffer().end());
  return out;
}

Result<ContainerTrailer> ParseContainerTrailer(std::span<const std::byte> trailer,
                                               uint64_t file_size, std::string_view name) {
  DDS_TRY(CheckMinimumSize(file_size, name));
  DDS_CHECK(trailer.size() == kContainerTrailerSize);
  const uint64_t at = file_size - kContainerTrailerSize;
  ContainerTrailer t;
  Reader r(trailer);
  std::span<const std::byte> magic;
  // The reads cannot fail: the span is exactly 32 bytes and the fields total 32.
  DDS_CHECK(r.U32(&t.footer_offset) && r.U32(&t.footer_length) && r.U32(&t.entry_count) &&
            r.U32(&t.footer_crc) && r.U32(&t.header_crc) && r.U64(&t.payload_bytes) &&
            r.Bytes(4, &magic));
  if (AsChars(magic) != kTrailerMagic) return Corrupt(name, at + 28, "bad trailer magic");
  // Every trailer field is cross-checked against the others and the file size (5.4).
  if (uint64_t{t.footer_offset} + t.footer_length + kContainerTrailerSize != file_size) {
    return Corrupt(name, at, "footer_offset + footer_length + 32 does not equal the file size");
  }
  if (t.footer_offset < kContainerHeaderSize ||
      t.payload_bytes != t.footer_offset - kContainerHeaderSize) {
    return Corrupt(name, at + 20, "payload_bytes does not match footer_offset");
  }
  // The entry size depends on the header's version, checked once the header is read; here the
  // length must fit one of them.
  const uint64_t v1_length = 4 + uint64_t{kFooterEntrySizeV1} * t.entry_count;
  const uint64_t v2_length = 4 + uint64_t{kFooterEntrySize} * t.entry_count;
  if (t.entry_count == 0 || (t.footer_length != v1_length && t.footer_length != v2_length)) {
    return Corrupt(name, at + 4, "footer_length does not match entry_count");
  }
  return t;
}

Result<ContainerMetadata> DecodeContainerParts(std::span<const std::byte> header,
                                               std::span<const std::byte> footer,
                                               const ContainerTrailer& trailer, uint64_t file_size,
                                               std::string_view name, uint32_t expected_id,
                                               const RepoId& repo_id, uint32_t max_data_size) {
  DDS_CHECK(header.size() == kContainerHeaderSize && footer.size() == trailer.footer_length);
  const uint64_t trailer_at = file_size - kContainerTrailerSize;
  // Header: its CRC first, so a damaged version field reads as corruption, not a new version.
  if (Crc32c(header) != trailer.header_crc) return Corrupt(name, trailer_at + 16, "header CRC");
  if (AsChars(header.first(8)) != kHeaderMagic) return Corrupt(name, 0, "bad header magic");
  const uint32_t version = LoadU32(header.data() + 8);
  if (version < kOldestContainerFormatVersion || version > kContainerFormatVersion) {
    return Status::Incompatible(StrCat(name, ": container format version ", version,
                                       " is not supported (this build reads versions 1 and 2)"));
  }
  const size_t entry_size = FooterEntrySize(version);
  if (trailer.footer_length != 4 + uint64_t{entry_size} * trailer.entry_count) {
    return Corrupt(name, trailer_at + 4, "footer_length does not match the version's entry size");
  }
  const uint32_t id = LoadU32(header.data() + 12);
  if (id != expected_id) {
    return Corrupt(name, 12,
                   StrCat("container id ", id, " differs from the file name's ", expected_id));
  }
  if (!std::equal(repo_id.begin(), repo_id.end(), header.begin() + 16,
                  [](uint8_t a, std::byte b) { return a == static_cast<uint8_t>(b); })) {
    return Corrupt(name, 16, "repo_id differs from the repository's config");
  }
  if (Crc32c(footer) != trailer.footer_crc) {
    return Corrupt(name, trailer.footer_offset, "footer CRC");
  }
  if (LoadU32(footer.data()) != trailer.entry_count) {
    return Corrupt(name, trailer.footer_offset, "entry_count differs from the trailer's");
  }
  ContainerMetadata meta{id, file_size, trailer.payload_bytes, {}};
  meta.entries.reserve(trailer.entry_count);
  std::unordered_set<Fingerprint, FingerprintHash> seen;
  uint64_t expected_offset = kContainerHeaderSize;
  Reader r(footer.subspan(4));
  for (uint32_t i = 0; i < trailer.entry_count; ++i) {
    const uint64_t entry_pos = trailer.footer_offset + 4 + uint64_t{entry_size} * i;
    // Sizes were checked against entry_count, so these reads cannot run out of footer.
    const RawEntry raw = ReadEntry(r, version, id);
    const SealedEntry& e = raw.entry;
    DDS_TRY(CheckEntry(raw, version, expected_offset, entry_pos, name, max_data_size));
    if (!seen.insert(e.fp).second) {
      return Corrupt(name, entry_pos, "fingerprint appears twice in one container");
    }
    expected_offset += e.loc.length;
    meta.entries.push_back(e);
  }
  if (expected_offset != trailer.footer_offset) {
    return Corrupt(name, trailer.footer_offset, "entries do not end at the footer");
  }
  return meta;
}

Result<ContainerMetadata> DecodeContainerMetadata(std::span<const std::byte> file,
                                                  std::string_view name, uint32_t expected_id,
                                                  const RepoId& repo_id, uint32_t max_data_size) {
  DDS_TRY(CheckMinimumSize(file.size(), name));
  auto trailer = ParseContainerTrailer(file.last(kContainerTrailerSize), file.size(), name);
  DDS_TRY(trailer.status());
  // The trailer's sizes were checked against file.size(), so these slices are in bounds.
  return DecodeContainerParts(file.first(kContainerHeaderSize),
                              file.subspan(trailer->footer_offset, trailer->footer_length),
                              *trailer, file.size(), name, expected_id, repo_id, max_data_size);
}

}  // namespace dds
