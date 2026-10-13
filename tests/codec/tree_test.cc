// Tree blob codec (ON_DISK_FORMAT.md section 6; TESTING.md 3.3) and the name rules (G10).

#include "dds/snapshot/tree.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "dds/common/encoding.h"
#include "dds/snapshot/names.h"
#include "support/test_seed.h"

namespace dds {
namespace {

Fingerprint Fp(uint8_t fill) {
  Fingerprint fp;
  fp.bytes.fill(fill);
  return fp;
}

TreeEntry File(std::string name, uint64_t size, std::vector<Fingerprint> pieces) {
  TreeEntry e;
  e.name = std::move(name);
  e.kind = EntryKind::kFile;
  e.mode = 0644;
  e.mtime_sec = 1700000000;
  e.mtime_nsec = 123456789;
  e.size = size;
  e.pieces = std::move(pieces);
  return e;
}

TreeEntry Dir(std::string name, Fingerprint subtree) {
  TreeEntry e;
  e.name = std::move(name);
  e.kind = EntryKind::kDirectory;
  e.mode = 0755;
  e.mtime_sec = -5;  // before 1970: mtimes are signed
  e.mtime_nsec = 0;
  e.subtree = subtree;
  return e;
}

TreeEntry Link(std::string name, std::string target) {
  TreeEntry e;
  e.name = std::move(name);
  e.kind = EntryKind::kSymlink;
  e.mode = 0777;
  e.mtime_sec = 1;
  e.mtime_nsec = 999999999;
  e.target = std::move(target);
  return e;
}

TEST(NamesTest, ValidNames) {
  EXPECT_TRUE(IsValidName("a"));
  EXPECT_TRUE(IsValidName("..."));
  EXPECT_TRUE(IsValidName(".hidden"));
  EXPECT_TRUE(IsValidName("caf\xc3\xa9"));
  EXPECT_TRUE(IsValidName("\xff\xfe"));  // arbitrary bytes are allowed (D41: names are bytes)
  EXPECT_TRUE(IsValidName(std::string(255, 'x')));
  EXPECT_FALSE(IsValidName(""));
  EXPECT_FALSE(IsValidName("."));
  EXPECT_FALSE(IsValidName(".."));
  EXPECT_FALSE(IsValidName("a/b"));
  EXPECT_FALSE(IsValidName(std::string("a\0b", 3)));
  EXPECT_FALSE(IsValidName(std::string(256, 'x')));
}

TEST(TreeTest, GoldenBytes) {
  Tree tree;
  tree.entries = {File("f", 3, {Fp(0xAA)}), Link("l", "t")};
  auto bytes = EncodeTree(tree);
  ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
  Writer w;
  w.U8(1);   // version
  w.U32(2);  // entry_count
  w.Str16("f");
  w.U8(1);  // kind: file
  w.U32(0644);
  w.I64(1700000000);
  w.U32(123456789);
  w.U64(3);  // size
  w.U32(1);  // piece_count
  w.Bytes(Fp(0xAA).AsBytes());
  w.Str16("l");
  w.U8(3);  // kind: symlink
  w.U32(0777);
  w.I64(1);
  w.U32(999999999);
  w.Str16("t");
  EXPECT_EQ(*bytes, w.buffer());
}

TEST(TreeTest, RoundTripIncludingEmptyAndLarge) {
  Tree empty;
  auto e = EncodeTree(empty);
  ASSERT_TRUE(e.ok());
  auto back = DecodeTree(*e);
  ASSERT_TRUE(back.ok());
  EXPECT_TRUE(back->entries.empty());

  for (uint64_t seed : testing::TestSeeds(3)) {
    SCOPED_TRACE(testing::SeedMessage(seed));
    std::mt19937_64 rng(seed);
    Tree tree;
    for (int i = 0; i < 300; ++i) {
      std::string name = "n" + std::to_string(i);
      if (i % 7 == 0) name += "\xe2\x82\xac";  // non-ASCII
      std::vector<Fingerprint> pieces(rng() % 5);
      for (auto& p : pieces) p = Fp(static_cast<uint8_t>(rng()));
      switch (rng() % 3) {
        case 0:  // a file's size is 0 exactly when it has no pieces
          tree.entries.push_back(File(name, pieces.empty() ? 0 : rng() | 1, pieces));
          break;
        case 1:
          tree.entries.push_back(Dir(name, Fp(static_cast<uint8_t>(rng()))));
          break;
        default:
          tree.entries.push_back(Link(name, std::string(1 + rng() % 4095, 'x')));
          break;
      }
    }
    std::sort(tree.entries.begin(), tree.entries.end(),
              [](const TreeEntry& a, const TreeEntry& b) { return a.name < b.name; });
    auto bytes = EncodeTree(tree);
    ASSERT_TRUE(bytes.ok()) << bytes.status().ToString();
    auto decoded = DecodeTree(*bytes);
    ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
    ASSERT_EQ(decoded->entries.size(), tree.entries.size());
    auto again = EncodeTree(*decoded);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(*again, *bytes);
  }
}

TEST(TreeTest, EncoderRejectsWhatTheFormatForbids) {
  Tree unsorted;
  unsorted.entries = {Link("b", "t"), Link("a", "t")};
  EXPECT_EQ(EncodeTree(unsorted).status().code(), StatusCode::kInvalidArgument);
  Tree dup;
  dup.entries = {Link("a", "t"), Link("a", "t")};
  EXPECT_EQ(EncodeTree(dup).status().code(), StatusCode::kInvalidArgument);
  Tree bad_name;
  bad_name.entries = {Link("a/b", "t")};
  EXPECT_EQ(EncodeTree(bad_name).status().code(), StatusCode::kInvalidArgument);
  Tree bad_mode;
  bad_mode.entries = {Link("a", "t")};
  bad_mode.entries[0].mode = 0100644;  // file type bits are not permission bits
  EXPECT_EQ(EncodeTree(bad_mode).status().code(), StatusCode::kInvalidArgument);
  Tree too_many_pieces;
  too_many_pieces.entries = {File("f", 1, std::vector<Fingerprint>(65537))};
  EXPECT_EQ(EncodeTree(too_many_pieces).status().code(), StatusCode::kInvalidArgument);
  Tree empty_target;
  empty_target.entries = {Link("a", "")};
  EXPECT_EQ(EncodeTree(empty_target).status().code(), StatusCode::kInvalidArgument);
}

// Raw tree bytes with one symlink entry named `name`, bypassing the encoder's checks, as a
// corrupted or hostile repository could contain.
std::vector<std::byte> RawTreeWithName(std::string_view name) {
  Writer w;
  w.U8(1);
  w.U32(1);
  w.Str16(name);
  w.U8(3);
  w.U32(0777);
  w.I64(0);
  w.U32(0);
  w.Str16("t");
  return w.Take();
}

TEST(TreeTest, DecoderRejectsHostileNames) {
  // G10: names like ".." or with '/' are corruption, so restore can never climb out.
  const std::string_view hostile[] = {"..", ".", "a/b", "/etc", "", std::string_view("a\0b", 3)};
  for (std::string_view name : hostile) {
    EXPECT_EQ(DecodeTree(RawTreeWithName(name)).status().code(), StatusCode::kCorruption)
        << "name '" << name << "'";
  }
}

TEST(TreeTest, UnknownVersionIsIncompatible) {
  auto bytes = RawTreeWithName("a");
  bytes[0] = std::byte{2};
  EXPECT_EQ(DecodeTree(bytes).status().code(), StatusCode::kIncompatible);
}

TEST(TreeTest, EverySingleBitFlipFailsOrReencodesExactly) {
  // Trees have no checksum of their own (BLAKE3 and the container CRC protect them), so a flip
  // must be rejected or decode to a tree that re-encodes to exactly the flipped bytes.
  Tree tree;
  tree.entries = {Dir("d", Fp(1)), File("f", 10, {Fp(2), Fp(3)}), Link("l", "target")};
  auto good = EncodeTree(tree);
  ASSERT_TRUE(good.ok());
  for (size_t byte = 0; byte < good->size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto flipped = *good;
      flipped[byte] ^= static_cast<std::byte>(1 << bit);
      auto decoded = DecodeTree(flipped);
      if (decoded.ok()) {
        auto again = EncodeTree(*decoded);
        ASSERT_TRUE(again.ok()) << "byte " << byte << " bit " << bit;
        EXPECT_EQ(*again, flipped) << "byte " << byte << " bit " << bit;
      } else {
        const StatusCode c = decoded.status().code();
        EXPECT_TRUE(c == StatusCode::kCorruption || c == StatusCode::kIncompatible);
      }
    }
  }
}

}  // namespace
}  // namespace dds
