#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dds {

// The three kinds of stored blob (SPEC.md section 1). The values are the type tags hashed into
// every fingerprint and stored in container footers (ON_DISK_FORMAT.md sections 1 and 5.3).
enum class BlobType : uint8_t { kData = 1, kTree = 2, kRecipe = 3 };

// Validates a type byte read from disk or the wire.
std::optional<BlobType> BlobTypeFromByte(uint8_t value);
// "data", "tree", "recipe".
const char* BlobTypeName(BlobType type);

// A blob's identity: BLAKE3-256 of (type tag ‖ payload) (D7). Equal fingerprints are treated as
// equal content, which is safe because BLAKE3 is a cryptographic hash (birthday bound about 2^128
// blobs). Byte 0 selects the index shard and bytes 8..15 the slot (D18).
struct Fingerprint {
  std::array<uint8_t, 32> bytes{};

  friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
  friend auto operator<=>(const Fingerprint&, const Fingerprint&) = default;

  // 64 lowercase hex digits.
  std::string ToHex() const;
  // Parses exactly 64 lowercase hex digits; nullopt otherwise.
  static std::optional<Fingerprint> FromHex(std::string_view hex);
  static Fingerprint FromBytes(std::span<const std::byte, 32> raw);
  std::span<const std::byte, 32> AsBytes() const {
    return std::span<const std::byte, 32>(reinterpret_cast<const std::byte*>(bytes.data()), 32);
  }

  // Bytes 8..15 as a little-endian u64. Fingerprints are uniformly random, so this needs no
  // further mixing to index a hash table (D18).
  uint64_t Hash64() const;
  uint8_t ShardByte() const { return bytes[0]; }
};

// For unordered containers keyed by Fingerprint.
struct FingerprintHash {
  size_t operator()(const Fingerprint& fp) const { return static_cast<size_t>(fp.Hash64()); }
};

// BLAKE3-256(type tag ‖ payload).
Fingerprint FingerprintOf(BlobType type, std::span<const std::byte> payload);

}  // namespace dds
