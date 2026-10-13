#include "dds/snapshot/manifest.h"

#include <algorithm>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "dds/hash/blake3_hasher.h"

namespace dds {

namespace {

constexpr std::string_view kMagic = "DDSSNAP1";

}  // namespace

SnapshotId SnapshotIdOf(std::span<const std::byte> manifest_bytes) {
  Blake3Hasher h;
  h.Update(manifest_bytes);
  const auto digest = h.Finalize();
  SnapshotId id{};
  std::copy_n(digest.begin(), id.size(), id.begin());
  return id;
}

std::string SnapshotIdToHex(const SnapshotId& id) {
  return HexEncode(std::as_bytes(std::span(id)));
}

std::optional<SnapshotId> SnapshotIdFromHex(std::string_view hex) {
  SnapshotId id{};
  if (!HexDecode(hex, std::as_writable_bytes(std::span(id)))) return std::nullopt;
  return id;
}

Status ValidateManifest(const Manifest& m) {
  if (m.host.size() > kMaxHostBytes || m.source_path.size() > kMaxSourcePathBytes ||
      m.tag.size() > kMaxTagBytes) {
    return Status::InvalidArgument(
        "manifest: host, source path, or tag is too long (limits 255, 4095, 1024 bytes)");
  }
  if (m.format_version < kOldestManifestFormatVersion ||
      m.format_version > kManifestFormatVersion) {
    return Status::InvalidArgument(StrCat("manifest: unknown format version ", m.format_version));
  }
  if (m.format_version == 1 && m.started_at_ns != 0) {
    return Status::InvalidArgument("manifest: version 1 has no start time");
  }
  return Status::Ok();
}

std::vector<std::byte> EncodeManifest(const Manifest& m) {
  DDS_CHECK(ValidateManifest(m).ok());
  Writer w;
  w.Bytes(AsBytes(kMagic));
  w.U32(m.format_version);
  w.Bytes(std::as_bytes(std::span(m.origin_repo_id)));
  w.I64(m.created_at_ns);
  if (m.format_version >= 2) w.I64(m.started_at_ns);
  w.Bytes(m.root.AsBytes());
  w.U64(m.logical_bytes);
  w.U64(m.file_count);
  w.U64(m.directory_count);
  w.U64(m.symlink_count);
  w.Str16(m.host);
  w.Str16(m.source_path);
  w.Str16(m.tag);
  w.U32(Crc32c(w.buffer()));
  return w.Take();
}

Result<Manifest> DecodeManifest(std::span<const std::byte> bytes) {
  if (bytes.size() < kMagic.size() + 4 + 4 || AsChars(bytes.first(kMagic.size())) != kMagic) {
    return Status::Corruption("manifest: bad magic or too short");
  }
  // The CRC covers everything before it, so a damaged version field reads as corruption.
  const auto body = bytes.first(bytes.size() - 4);
  if (Crc32c(body) != LoadU32(bytes.data() + body.size())) {
    return Status::Corruption("manifest: CRC mismatch");
  }
  Reader r(body.subspan(kMagic.size()));
  uint32_t version = 0;
  DDS_CHECK(r.U32(&version));  // length checked above
  if (version < kOldestManifestFormatVersion || version > kManifestFormatVersion) {
    return Status::Incompatible(StrCat("manifest format version ", version, " is not supported"));
  }
  Manifest m;
  m.format_version = version;
  std::span<const std::byte> origin;
  std::span<const std::byte> root;
  if (!r.Bytes(16, &origin) || !r.I64(&m.created_at_ns) ||
      (version >= 2 && !r.I64(&m.started_at_ns)) || !r.Bytes(32, &root) ||
      !r.U64(&m.logical_bytes) || !r.U64(&m.file_count) || !r.U64(&m.directory_count) ||
      !r.U64(&m.symlink_count) || !r.Str16(&m.host) || !r.Str16(&m.source_path) ||
      !r.Str16(&m.tag)) {
    return Status::Corruption("manifest: truncated");
  }
  if (!r.empty()) return Status::Corruption("manifest: unexpected bytes before the CRC");
  for (size_t i = 0; i < m.origin_repo_id.size(); ++i) {
    m.origin_repo_id[i] = static_cast<uint8_t>(origin[i]);
  }
  m.root = Fingerprint::FromBytes(root.first<32>());
  if (!ValidateManifest(m).ok()) return Status::Corruption("manifest: a string field is too long");
  return m;
}

}  // namespace dds
