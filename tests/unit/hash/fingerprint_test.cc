#include "dds/hash/fingerprint.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "dds/common/encoding.h"
#include "dds/hash/blake3_hasher.h"

namespace dds {
namespace {

Fingerprint FromIota(uint8_t start) {
  Fingerprint fp;
  for (size_t i = 0; i < fp.bytes.size(); ++i) fp.bytes[i] = static_cast<uint8_t>(start + i);
  return fp;
}

TEST(FingerprintTest, OfIsBlake3OverTagThenPayload) {
  const std::string payload = "hello, dedupstore";
  for (BlobType type : {BlobType::kData, BlobType::kTree, BlobType::kRecipe}) {
    std::vector<std::byte> tagged = {static_cast<std::byte>(type)};
    for (std::byte b : AsBytes(payload)) tagged.push_back(b);
    Blake3Hasher h;
    h.Update(tagged);
    EXPECT_EQ(FingerprintOf(type, AsBytes(payload)).bytes, h.Finalize());
  }
}

TEST(FingerprintTest, TypeTagSeparatesIdenticalPayloads) {
  // D7: a tree and a data chunk with identical bytes must not share an identity.
  const std::string payload = "same bytes";
  const Fingerprint data = FingerprintOf(BlobType::kData, AsBytes(payload));
  const Fingerprint tree = FingerprintOf(BlobType::kTree, AsBytes(payload));
  const Fingerprint recipe = FingerprintOf(BlobType::kRecipe, AsBytes(payload));
  EXPECT_NE(data, tree);
  EXPECT_NE(data, recipe);
  EXPECT_NE(tree, recipe);
}

TEST(FingerprintTest, TypeTagValues) {
  // ON_DISK_FORMAT.md section 1: 0x01 data, 0x02 tree, 0x03 recipe.
  EXPECT_EQ(static_cast<int>(BlobType::kData), 1);
  EXPECT_EQ(static_cast<int>(BlobType::kTree), 2);
  EXPECT_EQ(static_cast<int>(BlobType::kRecipe), 3);
  EXPECT_EQ(BlobTypeFromByte(1), BlobType::kData);
  EXPECT_EQ(BlobTypeFromByte(2), BlobType::kTree);
  EXPECT_EQ(BlobTypeFromByte(3), BlobType::kRecipe);
  EXPECT_EQ(BlobTypeFromByte(0), std::nullopt);
  EXPECT_EQ(BlobTypeFromByte(4), std::nullopt);
  EXPECT_EQ(BlobTypeFromByte(255), std::nullopt);
  EXPECT_STREQ(BlobTypeName(BlobType::kRecipe), "recipe");
}

TEST(FingerprintTest, HexRoundTrip) {
  const Fingerprint fp = FromIota(0xF0);
  const std::string hex = fp.ToHex();
  EXPECT_EQ(hex.size(), 64u);
  EXPECT_EQ(hex.substr(0, 8), "f0f1f2f3");
  EXPECT_EQ(Fingerprint::FromHex(hex), fp);
}

TEST(FingerprintTest, FromHexRejectsBadInput) {
  const std::string good = FromIota(0).ToHex();
  EXPECT_FALSE(Fingerprint::FromHex(good.substr(1)).has_value());
  EXPECT_FALSE(Fingerprint::FromHex(good + "00").has_value());
  std::string upper = good;
  upper[0] = 'A';
  EXPECT_FALSE(Fingerprint::FromHex(upper).has_value());
  EXPECT_FALSE(Fingerprint::FromHex("").has_value());
}

TEST(FingerprintTest, Hash64IsLittleEndianBytes8To15) {
  // D18: byte 0 selects the shard and bytes 8..15 index the slot.
  const Fingerprint fp = FromIota(0);  // bytes[i] = i
  EXPECT_EQ(fp.Hash64(), 0x0f0e0d0c0b0a0908ULL);
  EXPECT_EQ(fp.ShardByte(), 0);
  EXPECT_EQ(FromIota(7).ShardByte(), 7);
}

TEST(FingerprintTest, OrderingIsBytewise) {
  Fingerprint a = FromIota(0);
  Fingerprint b = a;
  b.bytes[31] = static_cast<uint8_t>(b.bytes[31] + 1);
  Fingerprint c = a;
  c.bytes[0] = 0xFF;
  EXPECT_LT(a, b);
  EXPECT_LT(b, c);
  EXPECT_EQ(a, FromIota(0));
}

TEST(FingerprintTest, FromBytesCopiesExactly) {
  std::vector<std::byte> raw(32);
  for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::byte>(100 + i);
  const Fingerprint fp = Fingerprint::FromBytes(std::span<const std::byte, 32>(raw.data(), 32));
  EXPECT_EQ(fp, FromIota(100));
  EXPECT_EQ(std::vector<std::byte>(fp.AsBytes().begin(), fp.AsBytes().end()), raw);
}

}  // namespace
}  // namespace dds
