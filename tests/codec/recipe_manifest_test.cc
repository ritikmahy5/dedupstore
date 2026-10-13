// Recipe blob (ON_DISK_FORMAT.md section 7) and snapshot manifest (section 8) codecs.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/hash/blake3_hasher.h"
#include "dds/snapshot/manifest.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/tree.h"
#include "support/test_seed.h"

namespace dds {
namespace {

Fingerprint Fp(uint8_t fill) {
  Fingerprint fp;
  fp.bytes.fill(fill);
  return fp;
}

TEST(RecipeTest, GoldenBytes) {
  Recipe r;
  r.refs = {{Fp(1), 100}, {Fp(2), 7}};
  Writer w;
  w.U8(1);
  w.U32(2);
  w.Bytes(Fp(1).AsBytes());
  w.U32(100);
  w.Bytes(Fp(2).AsBytes());
  w.U32(7);
  EXPECT_EQ(EncodeRecipe(r), w.buffer());
}

TEST(RecipeTest, RoundTripAndLimits) {
  Recipe max;
  for (int i = 0; i < 4096; ++i) max.refs.push_back({Fp(static_cast<uint8_t>(i)), 65536});
  auto back = DecodeRecipe(EncodeRecipe(max), 65536);
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ(back->refs.size(), 4096u);
  EXPECT_EQ(EncodeRecipe(*back), EncodeRecipe(max));
  // A length above the repository's max_size, a zero length, and 0 or 4097 refs are corruption.
  Recipe big;
  big.refs = {{Fp(1), 65537}};
  EXPECT_EQ(DecodeRecipe(EncodeRecipe(big), 65536).status().code(), StatusCode::kCorruption);
  Writer zero;
  zero.U8(1);
  zero.U32(1);
  zero.Bytes(Fp(1).AsBytes());
  zero.U32(0);
  EXPECT_EQ(DecodeRecipe(zero.buffer(), 65536).status().code(), StatusCode::kCorruption);
  Writer none;
  none.U8(1);
  none.U32(0);
  EXPECT_EQ(DecodeRecipe(none.buffer(), 65536).status().code(), StatusCode::kCorruption);
  Writer too_many;
  too_many.U8(1);
  too_many.U32(4097);
  EXPECT_EQ(DecodeRecipe(too_many.buffer(), 65536).status().code(), StatusCode::kCorruption);
}

TEST(RecipeTest, EverySingleBitFlipFailsOrReencodesExactly) {
  Recipe r;
  r.refs = {{Fp(5), 1000}, {Fp(6), 2000}};
  const auto good = EncodeRecipe(r);
  for (size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto flipped = good;
      flipped[byte] ^= static_cast<std::byte>(1 << bit);
      auto decoded = DecodeRecipe(flipped, 65536);
      if (decoded.ok()) {
        EXPECT_EQ(EncodeRecipe(*decoded), flipped) << "byte " << byte << " bit " << bit;
      } else {
        const StatusCode c = decoded.status().code();
        EXPECT_TRUE(c == StatusCode::kCorruption || c == StatusCode::kIncompatible);
      }
    }
  }
}

Manifest ExampleManifest() {
  Manifest m;
  for (size_t i = 0; i < m.origin_repo_id.size(); ++i) {
    m.origin_repo_id[i] = static_cast<uint8_t>(0xC0 + i);
  }
  m.created_at_ns = 1791331200000000000;
  m.started_at_ns = 1791331190000000000;
  m.root = Fp(0x77);
  m.logical_bytes = 123456;
  m.file_count = 10;
  m.directory_count = 3;
  m.symlink_count = 1;
  m.host = "laptop";
  m.source_path = "/home/ritik/data";
  m.tag = "nightly";
  return m;
}

// The fields after the timestamps, as ON_DISK_FORMAT.md section 8 lists them for both versions.
void AppendRest(Writer& w, const Manifest& m) {
  w.Bytes(m.root.AsBytes());
  w.U64(123456);
  w.U64(10);
  w.U64(3);
  w.U64(1);
  w.Str16("laptop");
  w.Str16("/home/ritik/data");
  w.Str16("nightly");
  w.U32(Crc32c(w.buffer()));
}

// Version 1 (before D106) still decodes and re-encodes to the same bytes, so the ids of snapshots
// written before version 2 never change.
TEST(ManifestTest, Version1GoldenBytesRoundTripUnchanged) {
  Manifest m = ExampleManifest();
  m.format_version = 1;
  m.started_at_ns = 0;
  Writer w;
  w.Bytes(AsBytes("DDSSNAP1"));
  w.U32(1);
  w.Bytes(std::as_bytes(std::span(m.origin_repo_id)));
  w.I64(m.created_at_ns);
  AppendRest(w, m);
  EXPECT_EQ(EncodeManifest(m), w.buffer());
  auto back = DecodeManifest(w.buffer());
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ(back->format_version, 1u);
  EXPECT_EQ(back->started_at_ns, 0);
  EXPECT_EQ(EncodeManifest(*back), w.buffer());
  m.started_at_ns = 5;  // a start time cannot be written in version 1
  EXPECT_FALSE(ValidateManifest(m).ok());
}

TEST(ManifestTest, GoldenBytesAndId) {
  const Manifest m = ExampleManifest();
  const auto bytes = EncodeManifest(m);
  Writer w;
  w.Bytes(AsBytes("DDSSNAP1"));
  w.U32(2);
  w.Bytes(std::as_bytes(std::span(m.origin_repo_id)));
  w.I64(m.created_at_ns);
  w.I64(m.started_at_ns);
  AppendRest(w, m);
  EXPECT_EQ(bytes, w.buffer());
  auto back = DecodeManifest(bytes);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->started_at_ns, m.started_at_ns);
  // The id is the first 16 bytes of BLAKE3 over the whole file, CRC included (section 8).
  Blake3Hasher h;
  h.Update(bytes);
  const auto digest = h.Finalize();
  const SnapshotId id = SnapshotIdOf(bytes);
  EXPECT_TRUE(std::equal(id.begin(), id.end(), digest.begin()));
  EXPECT_EQ(SnapshotIdToHex(id).size(), 32u);
  EXPECT_EQ(SnapshotIdFromHex(SnapshotIdToHex(id)), id);
  EXPECT_EQ(SnapshotIdFromHex("xyz"), std::nullopt);
}

TEST(ManifestTest, RoundTripWithEmptyAndMaximumStrings) {
  Manifest m = ExampleManifest();
  m.host = std::string(255, 'h');
  m.source_path = std::string(4095, 'p');
  m.tag = "";
  auto back = DecodeManifest(EncodeManifest(m));
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ(back->host, m.host);
  EXPECT_EQ(back->source_path, m.source_path);
  EXPECT_EQ(back->tag, "");
  EXPECT_EQ(back->root, m.root);
  EXPECT_EQ(EncodeManifest(*back), EncodeManifest(m));
}

TEST(ManifestTest, EverySingleBitFlipIsCorruptionOrIncompatible) {
  const auto good = EncodeManifest(ExampleManifest());
  for (size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto flipped = good;
      flipped[byte] ^= static_cast<std::byte>(1 << bit);
      auto decoded = DecodeManifest(flipped);
      ASSERT_FALSE(decoded.ok()) << "byte " << byte << " bit " << bit;
      const StatusCode c = decoded.status().code();
      EXPECT_TRUE(c == StatusCode::kCorruption || c == StatusCode::kIncompatible);
    }
  }
}

TEST(ManifestTest, OversizedStringsAreRejected) {
  Manifest m = ExampleManifest();
  m.tag = std::string(1025, 't');
  EXPECT_FALSE(ValidateManifest(m).ok());
}

// The fuzz seed corpora must hold valid inputs, or libFuzzer starts from rejected bytes and the
// manifest target, behind its CRC, explores almost nothing.
std::vector<std::vector<std::byte>> CorpusFiles(const std::string& target) {
  std::vector<std::vector<std::byte>> out;
  const std::filesystem::path dir =
      std::filesystem::path(DDS_FIXTURES_DIR) / "../../fuzz/corpus" / target;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    std::ifstream in(entry.path(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.emplace_back(AsBytes(bytes).begin(), AsBytes(bytes).end());
  }
  return out;
}

TEST(FuzzCorpusTest, SeedsAreValidInputs) {
  for (const auto& seed : CorpusFiles("tree")) EXPECT_TRUE(DecodeTree(seed).ok());
  for (const auto& seed : CorpusFiles("recipe")) EXPECT_TRUE(DecodeRecipe(seed, 65536).ok());
  for (const auto& seed : CorpusFiles("manifest")) EXPECT_TRUE(DecodeManifest(seed).ok());
  EXPECT_EQ(
      CorpusFiles("tree").size() + CorpusFiles("recipe").size() + CorpusFiles("manifest").size(),
      8u);
}

}  // namespace
}  // namespace dds
