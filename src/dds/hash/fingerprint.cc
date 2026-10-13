#include "dds/hash/fingerprint.h"

#include "dds/common/encoding.h"
#include "dds/common/strings.h"
#include "dds/hash/blake3_hasher.h"

namespace dds {

std::optional<BlobType> BlobTypeFromByte(uint8_t value) {
  switch (value) {
    case 1:
      return BlobType::kData;
    case 2:
      return BlobType::kTree;
    case 3:
      return BlobType::kRecipe;
    default:
      return std::nullopt;
  }
}

const char* BlobTypeName(BlobType type) {
  switch (type) {
    case BlobType::kData:
      return "data";
    case BlobType::kTree:
      return "tree";
    case BlobType::kRecipe:
      return "recipe";
  }
  return "unknown";
}

std::string Fingerprint::ToHex() const { return HexEncode(AsBytes()); }

std::optional<Fingerprint> Fingerprint::FromHex(std::string_view hex) {
  Fingerprint fp;
  if (!HexDecode(hex, std::as_writable_bytes(std::span(fp.bytes)))) return std::nullopt;
  return fp;
}

Fingerprint Fingerprint::FromBytes(std::span<const std::byte, 32> raw) {
  Fingerprint fp;
  for (size_t i = 0; i < fp.bytes.size(); ++i) fp.bytes[i] = static_cast<uint8_t>(raw[i]);
  return fp;
}

uint64_t Fingerprint::Hash64() const { return LoadU64(AsBytes().data() + 8); }

Fingerprint FingerprintOf(BlobType type, std::span<const std::byte> payload) {
  const auto tag = static_cast<std::byte>(type);
  Blake3Hasher hasher;
  hasher.Update(std::span(&tag, 1));
  hasher.Update(payload);
  Fingerprint fp;
  fp.bytes = hasher.Finalize();
  return fp;
}

}  // namespace dds
